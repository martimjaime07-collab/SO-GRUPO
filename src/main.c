#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>

#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"


typedef struct ThreadContext
{
	DataCenter *dc;          // Data Center shared by all threads.
	char **list;             // Names of the .conf files (sorted).
	size_t count;            // Number of .conf files.
	const char *dir;         // Folder of the .conf files (INPUT_DIR).
	size_t index;            // Next file to process (protected by mut).
	pthread_mutex_t mut;     // Protects index.
}ThreadContext;



static void process_conf_file(DataCenter *dc, const char *path){
	int fd = open(path, O_RDONLY);

		if(fd == -1){
			fprintf(stderr, "Failed to open file.\n");
			return;
		}
		int j = 1;

		while(j != 0){
			pthread_mutex_lock(&dc->lock);
			wait_for_all_vms(dc, WNOHANG);
			pthread_mutex_unlock(&dc->lock);
			switch (get_next_command(fd)){

			case CMD_DEFINE: {
				VMType vmtype;
				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}
				pthread_mutex_lock(&dc->lock);                  // 1. close the lock
    			int result = datacenter_define_VM(dc, &vmtype); // 2. access the Data Center
    			pthread_mutex_unlock(&dc->lock);                // 3. open it again
    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to define VM.\n");
        			continue;}
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
				pthread_mutex_lock(&dc->lock);                  // 1. close the lock
    			int result = datacenter_reserve(dc, &reservation); // 2. access the Data Center
    			pthread_mutex_unlock(&dc->lock);                // 3. open it again
    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to reserve VMs.\n");
        			continue;}
				printf("Reservation made successfully!\n");
				break;
			}

			case CMD_EXECUTE:{
				char id[MAX_STRING_SIZE];
				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}
				pthread_mutex_lock(&dc->lock);                  // 1. close the lock
    			int result = datacenter_execute(dc, id); // 2. access the Data Center
    			pthread_mutex_unlock(&dc->lock);                // 3. open it again
    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to execute reservation.\n");
        			continue;}
				printf("Finished reservation execution!\n");
				break;}
			
			case CMD_LIST:{
				pthread_mutex_lock(&dc->lock);                  // 1. close the lock
    			int result = datacenter_list(dc); // 2. access the Data Center
    			pthread_mutex_unlock(&dc->lock);                // 3. open it again
    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to list VMs.\n");
        			continue;}
				break;}

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
					pthread_mutex_lock(&dc->lock);
					wait_for_all_vms(dc, WNOHANG);
					pthread_mutex_unlock(&dc->lock);
				}
				break;}

			case CMD_INVALID:{
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;}

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
				break;}
			case CMD_EMPTY:{
				break;}
			case EOC:{
				j = 0;
				break;}
			}			
		}
		close(fd);
}

static void *work_thread(void *arg){

	ThreadContext *ctx = arg;
	while(1){

		pthread_mutex_lock(&ctx->mut);
		size_t idx = ctx->index  ;
		ctx->index = ctx-> index + 1;
		pthread_mutex_unlock(&ctx->mut);
		if (idx >= ctx->count)
			break;
		char path[MAX_PATH_SIZE];
		int snp = snprintf(path, MAX_PATH_SIZE,"%s/%s",ctx->dir, ctx->list[idx]);

		if (snp < 0 || (size_t)snp >= MAX_PATH_SIZE){
			fprintf(stderr, "Path too long: %s/%s\n", ctx->dir, ctx->list[idx]);
			continue;
	}
		process_conf_file(ctx->dc, path);
	}

	return NULL;

}

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 8) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <cores> <input_dir> <max_threads>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t cores;
	char *input_dir;
	size_t max_threads;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &cores) != 0 || cores == 0 ||
			path_exists(argv[6]) != 1 ||
			parse_size_t_arg(argv[7], &max_threads) != 0 || max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	input_dir = argv[6];
	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;}

	dc.local_cores = cores;
	char **buffer = NULL;

	ssize_t count = list_conf_files(input_dir, &buffer);

	if(count <= 0){
	    if(count == 0){
	        fprintf(stderr, "No .conf files\n");
	    } else {
	        fprintf(stderr, "Failed to list .conf files\n");
	    }
	    free(buffer);
	    return 1;}


	ThreadContext ctx;
	ctx.dc = &dc;
	ctx.list = buffer;
	ctx.count = (size_t) count;
	ctx.dir = input_dir;
	ctx.index = 0;
	pthread_mutex_init(&ctx.mut, NULL);
	size_t created = 0; //how many thread where created

	size_t n = max_threads < ctx.count ? max_threads : ctx.count;	
	pthread_t threads[n];

	for(size_t i = 0; i < n; i++){
		int thread_created = pthread_create(&threads[i], NULL, work_thread, &ctx);
		if (thread_created != 0){
			fprintf(stderr, "Failed to create thread: %s\n", strerror(thread_created));
			break;
		}
		created++;
	}

	for(size_t j = 0; j< created; j++){
		pthread_join(threads[j], NULL);
	}

	pthread_mutex_destroy(&ctx.mut);



	for(ssize_t i = 0; i < count; i++){
          free((buffer)[i]);}

	free(buffer);
	wait_for_all_vms(&dc, 0);
	datacenter_destroy(&dc);
	return 0;
}