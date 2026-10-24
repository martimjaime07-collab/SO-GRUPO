#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"

/* Data shared by all the worker threads. */
typedef struct {
	DataCenter *dc;
	const char *input_dir;
	char **files;            // names of the .conf files (sorted)
	size_t count;            // number of .conf files
	size_t next;             // index of the next file nobody took yet
	pthread_mutex_t next_lock; // protects 'next'
} WorkQueue;

/**
 * Executes all the commands of one .conf file.
 * The datacenter_* functions take the locks they need by themselves.
 */
static void process_file(DataCenter *dc, const char *input_dir, const char *name){
	char path[MAX_PATH_SIZE];
	int snp = snprintf(path, MAX_PATH_SIZE, "%s/%s", input_dir, name);

	if(snp < 0 || (size_t)snp >= MAX_PATH_SIZE){
		fprintf(stderr, "Path too long: %s/%s\n", input_dir, name);
		return;
	}

	// O_CLOEXEC: the VM processes should not inherit our open files.
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if(fd == -1){
		fprintf(stderr, "Failed to open file.\n");
		return;
	}

	int j = 1;

	while(j != 0){
		wait_for_all_vms(dc, WNOHANG);

		switch (get_next_command(fd)){
		case CMD_DEFINE: {
			VMType vmtype;

			if (parse_define(fd, &vmtype) != 0) {
				fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
				continue;
			}

			if (datacenter_define_VM(dc, &vmtype) != 0) {
				fprintf(stderr, "Failed to define VM.\n");
				continue;
			}

			printf("VM successfully defined!\n");
			break;
		}

		case CMD_RESERVE: {
			Reservation reservation = {0};

			size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

			if (num_items == 0) {
				fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
				continue;
			}

			if (datacenter_reserve(dc, &reservation) != 0) {
				fprintf(stderr, "Failed to reserve VM.\n");
				continue;
			}

			printf("Reservation made successfully!\n");
			break;
		}

		case CMD_EXECUTE:{
			char id[MAX_STRING_SIZE];

			if(parse_execute(fd, id) != 0){
				fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
				continue;
			}

			if (datacenter_execute(dc, id) != 0) {
				fprintf(stderr, "Failed to execute VM.\n");
				continue;
			}

			printf("Finished reservation execution!\n");
			break;
		}

		case CMD_LIST:{
			if (datacenter_list(dc) != 0) {
				fprintf(stderr, "Failed to list VM.\n");
				continue;
			}
			break;
		}

		case CMD_WAIT:{
			unsigned int delay;

			if(parse_wait(fd, &delay) != 0){
				fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
				continue;
			}

			unsigned int remaining = delay;
			while (remaining > 0) {
				unsigned int chunk = remaining > 50 ? 50 : remaining;
				datacenter_wait(chunk);
				remaining -= chunk;

				wait_for_all_vms(dc, WNOHANG);
			}
			break;
		}

		case CMD_INVALID:{
			fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
			break;
		}

		case CMD_HELP:{
			printf(
				"Spaces between arguments are allowed, but not after command end.\n"
				"Available commands:\n"
				" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
				" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
				" A <RESERVATION_ID>\n"
				" L\n"
				" E <DELAY_MS>\n"
				" H\n"
			);
			break;
		}

		case CMD_EMPTY:{
			break;
		}

		case EOC:{
			j = 0;
			break;
		}
		}
	}

	close(fd);
}

static void free_file_list(char **files, size_t count){
	for(size_t i = 0; i < count; i++){
		free(files[i]);
	}
	free(files);
}

/**
 * Worker thread: keeps taking the next .conf file until there are none left.
 */
static void *worker(void *arg){
	WorkQueue *queue = (WorkQueue *)arg;

	while(1){
		pthread_mutex_lock(&queue->next_lock);
		size_t i = queue->next;
		if(i < queue->count){
			queue->next++;
		}
		pthread_mutex_unlock(&queue->next_lock);

		if(i >= queue->count){
			break;
		}

		process_file(queue->dc, queue->input_dir, queue->files[i]);
	}

	return NULL;
}

int main(int argc, char **argv){

	if (argc != 8) {
		fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir> <max_threads>\n", argv[0]);
		return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t local_cores;
	char *input_dir;
	size_t max_threads;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &local_cores) != 0 || local_cores == 0 ||
			path_exists(argv[6]) != 1 ||
			parse_size_t_arg(argv[7], &max_threads) != 0 || max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	input_dir = argv[6];

	DataCenter dc;
	datacenter_init(&dc);

	Resources resources = {
		.ram = ram,
		.disk = disk,
		.cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	dc.local_cores = local_cores;

	char **buffer = NULL;

	ssize_t count = list_conf_files(input_dir, &buffer);

	if(count <= 0){
		if(count == 0){
			fprintf(stderr, "No .conf files\n");
		} else {
			fprintf(stderr, "Failed to list .conf files\n");
		}
		free(buffer);
		datacenter_destroy(&dc);
		return 1;
	}

	WorkQueue queue = {
		.dc = &dc,
		.input_dir = input_dir,
		.files = buffer,
		.count = (size_t)count,
		.next = 0
	};
	if(pthread_mutex_init(&queue.next_lock, NULL) != 0){
		fprintf(stderr, "Failed to initialize mutex.\n");
		free_file_list(buffer, queue.count);
		datacenter_destroy(&dc);
		return 1;
	}

	// No point in creating more threads than files.
	size_t num_threads = max_threads;
	if(num_threads > queue.count){
		num_threads = queue.count;
	}

	pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
	if(threads == NULL){
		fprintf(stderr, "Failed to allocate threads.\n");
		pthread_mutex_destroy(&queue.next_lock);
		free_file_list(buffer, queue.count);
		datacenter_destroy(&dc);
		return 1;
	}

	size_t created = 0;
	for(size_t i = 0; i < num_threads; i++){
		if(pthread_create(&threads[i], NULL, worker, &queue) != 0){
			fprintf(stderr, "Failed to create thread.\n");
			break;
		}
		created++;
	}

	if(created == 0){
		// could not create any thread: instead of failing, the main thread
		// does all the work (slower, but the result is the same)
		worker(&queue);
	}

	for(size_t i = 0; i < created; i++){
		if(pthread_join(threads[i], NULL) != 0){
			fprintf(stderr, "Failed to join thread %zu.\n", i);
		}
	}

	free(threads);
	pthread_mutex_destroy(&queue.next_lock);

	free_file_list(buffer, queue.count);

	// all threads are done, wait for the VMs that are still running
	wait_for_all_vms(&dc, 0);
	datacenter_destroy(&dc);
	return 0;
}
