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

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

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

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

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

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
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


// changed from here
void spawn_vm_child(DataCenter *dc, VM *vm, const char *res_id, const char *exec_abs) {
// to here

	// changed from here
	// Run the VM inside its own folder: /tmp/CloudIST/<res_id>/<vm_id>
	char vm_dir[MAX_PATH_SIZE];
	int n = snprintf(vm_dir, sizeof(vm_dir), CLOUDIST_BASE_DIR "/%s/%s", res_id, vm->id);
	if (n < 0 || (size_t)n >= sizeof(vm_dir) || chdir(vm_dir) == -1) {
		perror("chdir VM folder");
		_exit(1);
	}
	// to here
	struct rlimit lim;
		
	lim.rlim_cur = vm->type->required.ram << 30;
	lim.rlim_max = vm->type->required.ram << 30;
	if (setrlimit(RLIMIT_AS, &lim) == -1){
		perror("setrlimit RAM");
		_exit(1);
	}

	lim.rlim_cur = vm->type->required.disk << 30;
	lim.rlim_max = vm->type->required.disk << 30;
	if (setrlimit(RLIMIT_FSIZE, &lim) == -1){
		perror("setrlimit disk");
		_exit(1);
	}

	// changed from here
	// CPULIMIT = 100 * vCPUs-of-VM * local_cores / (CPUs-per-server * num_servers)
	double pct = 100.0 * vm->type->required.cpu * (double)dc->local_cores
						 / (vm->server->total.cpu * (double)dc->num_servers);
	int limit = (int)(pct + 0.5);
	if (limit < 1)	limit = 1;

	char limit_str[16];
	snprintf(limit_str, sizeof(limit_str), "%d", limit);

	char *cpu_lim[] = {"cpulimit", "-q", "-f", "-l", limit_str, "--", (char *)exec_abs, NULL};
	// to here
	execvp("cpulimit", cpu_lim);

	perror("execvp");
	_exit(1);
}

// changed from here
int spawn_all_vms(DataCenter *dc, Reservation *res) {
// to here
	// First loop: prepare /tmp/CloudIST/<res>/<vm> for every VM (Exercise 1).
	for (size_t i = 0; i < res->num_vms; i++) {
		
		VM *vm = res->vms[i];
		int check = create_dir(res-> id, vm->id, vm->type->input_folder);

		if(check == 0){
			continue;
		}
		else{return 1;}
	}

	// Second loop: launch one process per VM (Exercise 2, in progress).
	for (size_t i = 0; i < res->num_vms; i++) {

		VM *vm = res->vms[i];

		// changed from here
		// Resolve the executable to an absolute path BEFORE the child does chdir().
		char exec_abs[MAX_PATH_SIZE];
		if (absolute_path(vm->type->exec_path, exec_abs, sizeof(exec_abs)) != 0) {
			fprintf(stderr, "Could not resolve executable path %s\n", vm->type->exec_path);
			return 1;
		}
		// to here

		pid_t pid = fork();

		if (pid < 0) {
            perror("fork");
            return 1;                 // handle error, kill previous child and return
        }
		else if(pid == 0){
			//child
			// changed from here
			spawn_vm_child(dc, vm, res->id, exec_abs);
			// to here
		}
		else{
			//parent saves pid
			vm->pid = pid;
			vm->state = VM_STATE_RUNNING;
		}

	}

	return 0;
}

int destroy_vm_by_pid(DataCenter *dc, pid_t pid) {
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];

		for (size_t k = 0; k < res->num_vms; k++) {
			if (res->vms[k]->pid != pid)
				continue;

			VM *vm = res->vms[k];
			Server *server = vm->server;

			// 1. Return the VM resources back to the hosting server.
			resources_add(&server->available, vm->type->required);

			// 2. Remove the VM from the server's hosted VM list.
			for (size_t j = 0; j < server->num_hosted_vms; j++) {
				if (server->hosted_vms[j] == vm) {
					memmove(&server->hosted_vms[j],
									&server->hosted_vms[j + 1],
									(server->num_hosted_vms - j - 1) * sizeof(VM *));
					server->num_hosted_vms--;
					break;
				}
			}

			// 3. Remove the VM from the reservation.
			memmove(&res->vms[k], &res->vms[k + 1],(res->num_vms - k - 1) * sizeof(VM *));
			res->num_vms--;

			// 4. Free the VM.
			free(vm);

			// 5. If it was the last VM, remove the whole reservation.
			if (res->num_vms == 0)
				reservation_destroy(dc, res);

			return 0;
		}
	}

	return 1;
}

void wait_for_all_vms(DataCenter *dc, int options) {
	while (1) {
		int status;
		pid_t pid = waitpid(-1, &status, options);

		if (pid > 0) {             // a VM finished: destroy it, look for more
			destroy_vm_by_pid(dc, pid);
			continue;
		}

		if (pid == 0)              // WNOHANG: none has finished yet
			break;

		if (errno == EINTR)        // interrupted by a signal: try again
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