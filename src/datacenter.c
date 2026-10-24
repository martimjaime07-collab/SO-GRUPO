#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;

  dc->num_reservations = 0;

  dc->configured = 0;
  dc->local_cores = 0;

  pthread_rwlock_init(&dc->vm_types_lock, NULL);
  pthread_mutex_init(&dc->lock, NULL);
  pthread_mutex_init(&dc->proc_lock, NULL);
}

void datacenter_destroy(DataCenter *dc) {
  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
  }
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_destroy(&dc->servers[i].lock);
  }
  free(dc->servers);
  pthread_rwlock_destroy(&dc->vm_types_lock);
  pthread_mutex_destroy(&dc->lock);
  pthread_mutex_destroy(&dc->proc_lock);
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    pthread_mutex_init(&dc->servers[i].lock, NULL);
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

/* Caller must hold vm_types_lock for writing. */
static int define_VM_locked(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if(VMType_exists(dc, type->id) != NULL){
    fprintf(stderr, "VM type with id %s already exists.\n", type->id);
    return 1;
  }

  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  if(dc->num_vm_types >= MAX_VM_TYPES){
    fprintf(stderr, "Maximum of VM types reached.\n");
    return 1;
  }
  
  dc->vm_types[dc->num_vm_types++] = *type;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  pthread_rwlock_wrlock(&dc->vm_types_lock);
  int result = define_VM_locked(dc, type);
  pthread_rwlock_unlock(&dc->vm_types_lock);
  return result;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // needed[i] = 1 if the reservation uses server i+1
  int *needed = calloc(dc->num_servers, sizeof(int));
  if (needed == NULL) {
    fprintf(stderr, "Failed to allocate memory.\n");
    return 1;
  }

  // 1. Check the servers and find the VM type of every item. The types are
  // only read, so many threads can do this at the same time. We let go of the
  // lock right after: the VMType pointers stay valid (types are never removed).
  int valid = 1;

  pthread_rwlock_rdlock(&dc->vm_types_lock);
  for (size_t i = 0; i < reservation->items_count; i++) {
    ReservationItem *item = &reservation->items[i];

    if (item->server_id == 0 || item->server_id > dc->num_servers) {
      fprintf(stderr, "Invalid server in reservation.\n");
      valid = 0;
      break;
    }

    item->vm_type = VMType_exists(dc, item->vm_type_id);
    if (item->vm_type == NULL) {
      fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
      valid = 0;
      break;
    }

    needed[item->server_id - 1] = 1;
  }
  pthread_rwlock_unlock(&dc->vm_types_lock);

  if (!valid) {
    free(needed);
    return 1;
  }

  // 2. Lock ONLY the servers we use, always from the lowest id to the highest
  // (same order for everyone => no deadlock).
  for (size_t i = 0; i < dc->num_servers; i++) {
    if (needed[i]) pthread_mutex_lock(&dc->servers[i].lock);
  }

  int result = 1;

  if (reservation_validate(dc, reservation) == 0 &&
      reservation_commit(dc, reservation) == 0) {

    // 3. Only now we take the global lock, just to put the reservation in the list.
    // This is also where we check if the id already exists (another thread may
    // have inserted the same id).
    pthread_mutex_lock(&dc->lock);

    int duplicated = 0;
    for (size_t i = 0; i < dc->num_reservations; i++) {
      if (strncmp(dc->reservations[i].id, reservation->id, MAX_STRING_SIZE) == 0) {
        duplicated = 1;
        break;
      }
    }

    if (duplicated) {
      fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
      reservation_rollback(reservation, 0);
    } else if (dc->num_reservations >= MAX_RESERVATIONS) {
      fprintf(stderr, "Max number of reservations reached\n");
      reservation_rollback(reservation, 0);
    } else {
      dc->reservations[dc->num_reservations] = *reservation;
      dc->num_reservations++;
      result = 0;
    }

    pthread_mutex_unlock(&dc->lock);
  }

  for (size_t i = 0; i < dc->num_servers; i++) {
    if (needed[i]) pthread_mutex_unlock(&dc->servers[i].lock);
  }

  free(needed);
  return result;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // Under the lock we only find the reservation and take a copy of what we
  // need. The slow part (copying files, fork) is done without dc->lock.
  // (We can not keep the Reservation pointer: when another reservation is
  // destroyed the array is shifted and the pointer would point to a wrong one.)
  VM *vms[MAX_RESERVATION_VMS];
  size_t num_vms;
  char res_id[MAX_STRING_SIZE];

  pthread_mutex_lock(&dc->lock);

  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) {
    pthread_mutex_unlock(&dc->lock);
    return 1;
  }

  res->state = RES_STATE_RUNNING; // so no other thread can execute it again
  num_vms = res->num_vms;
  memcpy(vms, res->vms, num_vms * sizeof(VM *));
  snprintf(res_id, sizeof(res_id), "%s", res->id);

  pthread_mutex_unlock(&dc->lock);

  if (spawn_all_vms(dc, res_id, vms, num_vms) != 0) return 1;

  return 0;
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // Lock every server (lowest id first) and then the global lock, so the
  // listing is a consistent snapshot. flockfile makes the whole listing
  // come out together even if other threads are printing.
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_lock(&dc->servers[i].lock);
  }
  pthread_mutex_lock(&dc->lock);
  flockfile(stdout);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server *server = &dc->servers[i];

    Resources used = server->total;
    resources_sub(&used, server->available);

    printf(" Server #%zu\n", server->id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->total.cpu, server->total.ram, server->total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->available.cpu, server->available.ram, server->available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server->num_hosted_vms);

    if (server->num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server->num_hosted_vms; j++) {
        VM *vm = server->hosted_vms[j];
        const char *prefix = (j == server->num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");

  funlockfile(stdout);
  pthread_mutex_unlock(&dc->lock);
  for (size_t i = dc->num_servers; i > 0; i--) {
    pthread_mutex_unlock(&dc->servers[i - 1].lock);
  }
  pthread_rwlock_unlock(&dc->vm_types_lock);
  return 0;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}