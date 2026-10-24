#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include <sys/resource.h>

#include "datacenter.h"

/**
 * Checks if a VMType with the given id exists.
 * 
 * The caller must hold vm_types_lock (for reading or for writing).
 *
 * @param dc Pointer to Data Center.
 * @param type_id Id of VM type being checked.
 * 
 * @return Pointer to VMType if it exists.
 * @return NULL if VMType does not exist.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Simulates the placement of every VM requested in the reservation using a
 * temporary copy of each server's available resources. The Data Center state
 * is left unchanged regardless of the outcome.
 *
 * Validation fails if any requested server does not have enough available
 * resources to host all the requested VMs assigned to it or if the request 
 * does not have valid parameters (VM types that do not exist, invalid server ids).
 *
 * The caller must hold the lock of every server used by the reservation, and
 * item->vm_type must already be filled (done by datacenter_reserve). The
 * check for a repeated reservation id is NOT done here, it is done when the
 * reservation is inserted in the Data Center (datacenter_reserve).
 *
 * @param dc Data Center where the reservation will be validated.
 * @param reservation Reservation to validate.
 *
 * @return 0 if the reservation can be fully allocated.
 * @return 1 otherwise.
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

/**
 * Creates all VMs requested by the reservation, assigns them to their
 * designated servers, updates the server resource availability, and stores
 * references to the created VMs in both the reservation and the corresponding
 * server hosted VM list.
 *
 * The reservation is expected to have already passed validation before this
 * function is called. If an error occurs during the commit process, the
 * function performs a rollback of the changes made by this operation.
 *
 * @param dc Data Center where the reservation will be committed.
 * @param reservation Reservation to allocate.
 *
 * @return 0 if the reservation was successfully committed
 * @return 1 if an error occurred and the changes were rolled back.
 */
int reservation_commit(DataCenter *dc, Reservation *reservation);

/**
 * Undoes reservation_commit (gives the resources back and frees the VMs).
 * The caller must hold the locks of the servers used by the reservation.
 */
void reservation_rollback(Reservation *reservation, size_t initial_vms);

/**
 * Looks up a reservation by id and checks that it is pending.
 * The caller must hold dc->lock, and the returned pointer is only valid while
 * it keeps holding it (the array is shifted when a reservation is removed).
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to look up.
 *
 * @return Pointer to the Reservation if found and RES_STATE_PENDING.
 * @return NULL if not found or not in RES_STATE_PENDING (error is printed).
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Frees every VM allocated to the reservation, returns their resources to the
 * corresponding hosting servers, removes them from each server's hosted VM
 * list, and finally removes the reservation from the Data Center reservation
 * list.
 *
 * @param dc Pointer to the Data Center containing the reservation.
 * @param reservation Pointer to the reservation to destroy.
 */
void reservation_destroy(DataCenter *dc, Reservation *reservation);

/**
 * Everything the child process needs after fork(). It is filled by the parent
 * BEFORE the fork, because in a multi-threaded program the child may only call
 * simple functions (chdir, setrlimit, execvp, write, _exit) until the exec.
 */
typedef struct SpawnInfo {
  char vm_dir[MAX_PATH_SIZE];   // /tmp/CloudIST-<id>/<res_id>/<vm_id>
  char exec_abs[MAX_PATH_SIZE]; // absolute path of the executable
  char limit_str[16];           // CPULIMIT percentage, already as a string
  rlim_t ram;                   // RLIMIT_AS value in bytes
  rlim_t disk;                  // RLIMIT_FSIZE value in bytes
} SpawnInfo;

/**
 * Runs in the child process after fork(): goes to the VM folder, sets the
 * resource limits (RAM, disk) and execs the workload under cpulimit. Never
 * returns (either execs successfully or exits with 1).
 *
 * The CPU percentage given to cpulimit (already inside info) is:
 * 100 * vCPUs-of-VM * local_cores / (CPUs-per-server * num_servers)
 *
 * @param info Values prepared by the parent before the fork.
 */
_Noreturn void spawn_vm_child(const SpawnInfo *info);

/**
 * Prepares the folder of every VM and forks one child per VM, calling
 * spawn_vm_child() in each child and recording the pid/state in the parent.
 * Works on a copy of the VM list so it does not need dc->lock while it copies
 * files (that is the slow part).
 *
 * @param dc Pointer to the Data Center.
 * @param res_id Id of the reservation.
 * @param vms Array with the VMs of the reservation.
 * @param n Number of VMs in the array.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int spawn_all_vms(DataCenter *dc, const char *res_id, VM **vms, size_t n);

/**
 * Destroys the VM whose process has the given pid: returns its resources to
 * the hosting server, removes it from the server's hosted VM list and from its
 * reservation, and frees it. If it was the last VM of the reservation, the
 * reservation is removed from the Data Center.
 *
 * @return 0 if the VM was found and destroyed, 1 otherwise.
 */
int destroy_vm_by_pid(DataCenter *dc, pid_t pid);

/**
 * Collects the processes of VMs that have finished (from any reservation) and
 * destroys each one with destroy_vm_by_pid().
 * options = WNOHANG: only collects the ones already finished, never blocks.
 * options = 0: blocks until every VM has finished (only use it when no other
 * thread is running, because it holds proc_lock while it waits).
 */
void wait_for_all_vms(DataCenter *dc, int options);

/**
 * Converts a VM state to a human-readable string. Used by datacenter_list().
 *
 * @param state The VMState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *vm_state_to_string(VMState state);

/**
 * Converts a reservation state to a human-readable string. Used by
 * datacenter_list().
 *
 * @param state The ReservationState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H