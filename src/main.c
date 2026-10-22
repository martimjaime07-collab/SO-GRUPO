#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 6) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char *input_dir;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			path_exists(argv[5]) != 1) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	input_dir = argv[5];
	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	char **buffer = NULL;

	ssize_t count = list_conf_files(input_dir, &buffer);

	if(count <= 0){
	    if(count == 0){
	        fprintf(stderr, "No .conf files\n");
	    } else {
	        fprintf(stderr, "Failed to list .conf files\n");
	    }
	    free(buffer);
	    return 1;
}

	for(int i = 0; i < count; i++){
		char path[MAX_PATH_SIZE];
		int snp = snprintf(path, MAX_PATH_SIZE,"%s/%s",input_dir, buffer[i]);

		if(snp < 0 || (size_t)snp >= MAX_PATH_SIZE){
			fprintf(stderr, "Path too long: %s/%s\n", input_dir, buffer[i]);
			continue;
		}

		int fd = open(path, O_RDONLY);

		if(fd == -1){
			fprintf(stderr, "Failed to open file.\n");
			continue;
		}
		int j = 1;

		while(j != 0){
			switch (get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				pthread_mutex_lock(&dc.lock);                  // 1. close the lock
    			int result = datacenter_define_VM(&dc, &vmtype); // 2. access the Data Center
    			pthread_mutex_unlock(&dc.lock);                // 3. open it again

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

				pthread_mutex_lock(&dc.lock);                  // 1. close the lock
    			int result = datacenter_reserve(&dc, &reservation); // 2. access the Data Center
    			pthread_mutex_unlock(&dc.lock);                // 3. open it again

    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to reserve VM.\n");
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

				pthread_mutex_lock(&dc.lock);                  // 1. close the lock
    			int result = datacenter_execute(&dc, id); // 2. access the Data Center
    			pthread_mutex_unlock(&dc.lock);                // 3. open it again

    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to execute VM.\n");
        			continue;}


				printf("Finished reservation execution!\n");

				break;}
			
			case CMD_LIST:{
				pthread_mutex_lock(&dc.lock);                  // 1. close the lock
    			int result = datacenter_list(&dc); // 2. access the Data Center
    			pthread_mutex_unlock(&dc.lock);                // 3. open it again

    			// 4. only now test the result: the continue can no longer skip the unlock
    			if (result != 0) {
        			fprintf(stderr, "Failed to list VM.\n");
        			continue;}

				break;}

			case CMD_WAIT:{
				unsigned int delay;

				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait(delay);
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
	for(ssize_t i = 0; i < count; i++){
          free((buffer)[i]);
        }
	free(buffer);
	datacenter_destroy(&dc);
	return 0;
}