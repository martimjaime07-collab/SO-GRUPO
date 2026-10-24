#include "datacenter_utils.h"
#include "filesystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		// already resolved by datacenter_reserve
		VMType *vm_type = item->vm_type;

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}


/* Writes a message to stderr using only write() (safe after fork). */
#define CHILD_FAIL(msg) do { \
	ssize_t r_ = write(STDERR_FILENO, msg, sizeof(msg) - 1); \
	(void)r_; \
	_exit(1); \
} while (0)

/**
 * Fills everything the child needs BEFORE the fork, so that after the fork
 * the child only calls chdir/setrlimit/execvp/write/_exit.
 */
static int prepare_spawn_info(DataCenter *dc, VM *vm, const char *res_id, SpawnInfo *info) {
	int n = snprintf(info->vm_dir, sizeof(info->vm_dir), CLOUDIST_BASE_DIR "/%s/%s", res_id, vm->id);
	if (n < 0 || (size_t)n >= sizeof(info->vm_dir)) {
		fprintf(stderr, "VM folder path too long.\n");
		return 1;
	}

	// Resolve the executable to an absolute path BEFORE the child does chdir().
	if (absolute_path(vm->type->exec_path, info->exec_abs, sizeof(info->exec_abs)) != 0) {
		fprintf(stderr, "Could not resolve executable path %s\n", vm->type->exec_path);
		return 1;
	}

	info->ram = vm->type->required.ram << 30;
	info->disk = vm->type->required.disk << 30;

	// CPULIMIT = 100 * vCPUs-of-VM * local_cores / (CPUs-per-server * num_servers)
	double pct = 100.0 * vm->type->required.cpu * (double)dc->local_cores
	             / (vm->server->total.cpu * (double)dc->num_servers);
	int limit = (int)(pct + 0.5);
	if (limit < 1)	limit = 1;
	snprintf(info->limit_str, sizeof(info->limit_str), "%d", limit);

	return 0;
}

_Noreturn void spawn_vm_child(const SpawnInfo *info) {
	// Run the VM inside its own folder: /tmp/CloudIST-<id>/<res_id>/<vm_id>
	if (chdir(info->vm_dir) == -1)
		CHILD_FAIL("cloudIST: chdir to the VM folder failed\n");

	struct rlimit lim;

	lim.rlim_cur = info->ram;
	lim.rlim_max = info->ram;
	if (setrlimit(RLIMIT_AS, &lim) == -1)
		CHILD_FAIL("cloudIST: setrlimit (RAM) failed\n");

	lim.rlim_cur = info->disk;
	lim.rlim_max = info->disk;
	if (setrlimit(RLIMIT_FSIZE, &lim) == -1)
		CHILD_FAIL("cloudIST: setrlimit (disk) failed\n");

	char *cpu_lim[] = {"cpulimit", "-q", "-f", "-l", (char *)info->limit_str, "--", (char *)info->exec_abs, NULL};
	execvp("cpulimit", cpu_lim);

	CHILD_FAIL("cloudIST: could not run cpulimit\n");
}

/**
 * Gives back the resources of a VM, removes it from its server and from its
 * reservation and frees it. If it was the last VM of the reservation, the
 * reservation is removed too.
 *
 * Used for VMs that finished and for VMs that were never started.
 */
static int release_vm(DataCenter *dc, VM *vm) {
	// Step 1: give the resources back (only the lock of that server).
	// We can not keep dc->lock while taking the server lock (the order is server -> dc).
	Server *server = vm->server;

	pthread_mutex_lock(&server->lock);
	resources_add(&server->available, vm->type->required);
	for (size_t j = 0; j < server->num_hosted_vms; j++) {
		if (server->hosted_vms[j] == vm) {
			memmove(&server->hosted_vms[j],
							&server->hosted_vms[j + 1],
							(server->num_hosted_vms - j - 1) * sizeof(VM *));
			server->num_hosted_vms--;
			break;
		}
	}
	pthread_mutex_unlock(&server->lock);

	// Step 2: remove it from its reservation. The reservation is searched
	// with the lock taken (the array can be shifted when other reservations are removed).
	pthread_mutex_lock(&dc->lock);
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];

		for (size_t k = 0; k < res->num_vms; k++) {
			if (res->vms[k] != vm)
				continue;

			memmove(&res->vms[k], &res->vms[k + 1], (res->num_vms - k - 1) * sizeof(VM *));
			res->num_vms--;
			free(vm);

			if (res->num_vms == 0)
				reservation_destroy(dc, res);

			pthread_mutex_unlock(&dc->lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&dc->lock);

	return 1;
}

int spawn_all_vms(DataCenter *dc, const char *res_id, VM **vms, size_t n) {
	int error = 0;
	size_t started = 0; // how many VMs already have a process

	// First loop: prepare /tmp/CloudIST-<id>/<res>/<vm> for every VM (Exercise 1).
	for (size_t i = 0; i < n && !error; i++) {
		if (create_dir(res_id, vms[i]->id, vms[i]->type->input_folder) != 0) {
			error = 1;
		}
	}

	// Second loop: launch one process per VM (Exercise 2).
	while (!error && started < n) {
		VM *vm = vms[started];
		SpawnInfo info;

		if (prepare_spawn_info(dc, vm, res_id, &info) != 0) {
			error = 1;
			break;
		}

		// proc_lock makes fork + "save the pid" one single step. Without it
		// another thread could call waitpid, get this pid and not find the VM.
		pthread_mutex_lock(&dc->proc_lock);
		pthread_mutex_lock(&dc->lock);

		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			pthread_mutex_unlock(&dc->lock);
			pthread_mutex_unlock(&dc->proc_lock);
			error = 1;
			break;
		}
		else if (pid == 0) {
			// child: everything it needs is already in 'info'
			spawn_vm_child(&info);
		}

		vm->pid = pid;
		vm->state = VM_STATE_RUNNING;
		started++;

		pthread_mutex_unlock(&dc->lock);
		pthread_mutex_unlock(&dc->proc_lock);
	}

	if (error) {
		// The VMs that were already started keep running (they are registered
		// and will be removed when they finish). The ones that were never
		// started are cancelled, otherwise they would keep their resources forever.
		for (size_t i = started; i < n; i++) {
			release_vm(dc, vms[i]);
		}
		return 1;
	}

	return 0;
}

int destroy_vm_by_pid(DataCenter *dc, pid_t pid) {
	VM *vm = NULL;

	// Find the VM that has this pid (global lock only).
	pthread_mutex_lock(&dc->lock);
	for (size_t i = 0; i < dc->num_reservations && vm == NULL; i++) {
		Reservation *res = &dc->reservations[i];
		for (size_t k = 0; k < res->num_vms; k++) {
			if (res->vms[k]->pid == pid) {
				vm = res->vms[k];
				break;
			}
		}
	}
	pthread_mutex_unlock(&dc->lock);

	if (vm == NULL)
		return 1;

	// Only the thread that reaped this pid gets here for this VM, so nobody
	// else frees it between the search and release_vm.
	return release_vm(dc, vm);
}

void wait_for_all_vms(DataCenter *dc, int options) {
	while (1) {
		int status;

		pthread_mutex_lock(&dc->proc_lock);
		pid_t pid = waitpid(-1, &status, options);
		int saved_errno = errno;
		pthread_mutex_unlock(&dc->proc_lock);

		if (pid > 0) {             // a VM finished: destroy it, look for more
			destroy_vm_by_pid(dc, pid);
			continue;
		}

		if (pid == 0)              // WNOHANG: none has finished yet
			break;

		if (saved_errno == EINTR)  // interrupted by a signal: try again
			continue;

		break;                     // ECHILD: no more VMs running
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}