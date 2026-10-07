#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define BUFFER_SIZE 4096

#include "filesystem.h"
#include "constants.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>


int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}



int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}



static int compare_strings(const void *a, const void *b){
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa,sb);
  }




ssize_t list_conf_files(const char *path, char ***buffer){

  *buffer = NULL; 

  DIR *dir = opendir(path);
  

  if(dir == NULL){
    perror("error opening dir \n");
    return -1;
  }

  struct dirent *file;
  size_t count = 0;
  size_t capacity = 8;

  *buffer = malloc(capacity * sizeof(char *));

  if(*buffer == NULL){
    closedir(dir);
    return -1;
  }

  errno = 0;  
  while ((file = readdir(dir)) != NULL){

    size_t file_size = strlen(file->d_name);
    
    /**
     * We use < 6 to make sure that the file is at least a.conf, we dont want just .conf
     */
    if(file_size < 6){continue;} 

    /**
     * If the last 5 characters are not ".conf" we go checkout the next file
     */
    if(strcmp(file->d_name + file_size - 5,".conf") != 0) {continue;}

    if(count == capacity){

      capacity *= 2;

      char **tmp = realloc(*buffer, capacity * sizeof(char*));

      if(tmp == NULL){
        perror("realloc error\n");
        for(size_t i = 0; i < count; i++){
          free((*buffer)[i]);
        }

        free(*buffer);
        *buffer = NULL;
        closedir(dir);
        return -1;
      }
      
      *buffer = tmp;
    }

    char *dup = strdup(file->d_name);

    if(dup == NULL){
      perror("strdup error\n");
      for(size_t i = 0; i < count; i++){
          free((*buffer)[i]);
        }
      free(*buffer);
      closedir(dir);
      *buffer = NULL;
      return -1;
    }
    (*buffer)[count++] = dup; 
    errno = 0;
    
  }

  /* We save errno because we call closedir next and it can change the previous errno */

  int readdir_errno = errno;

  closedir(dir);
  
  if(readdir_errno == 0){
    
    if(count > 0){
      qsort(*buffer, count, sizeof(char *), compare_strings);
    }   
    
    return (ssize_t)count;
  }
  
  else{
    
    for(size_t i = 0; i < count; i++){
          free((*buffer)[i]);
        }
    free(*buffer);
    *buffer = NULL;
    return -1;}
}




int aux_snprintf(char *buffer, size_t sizeof_buffer,const char *message){
  
  size_t size = strlen(buffer);

  if(size >= sizeof_buffer){
    fprintf(stderr, "buffer without space\n");
    return -1;
  }
  

  int snp = snprintf(buffer + size, sizeof_buffer - size, "/%s", message);
  
  if(snp < 0){
    fprintf(stderr, "snprintf error\n");
    return -1;}


  else if((size_t)snp >= sizeof_buffer - size){
    fprintf(stderr, "snprintf truncated\n");
    return -1;
  }
  return 0;

}



int aux_mkdir(const char *buffer){
  
  int check = mkdir(buffer, 0777);
  
  if(check == -1){
    if(errno == EEXIST){/*we just want to make sure the errno is EEXIST(already exists)*/}
    else{
      perror("mkdir error");
      return -1;
    }
  }
  return 0;
}


int open_dir(char *src, char *dest){

  size_t size_src = strlen(src);
  size_t size_dest = strlen(dest);

  DIR *dir = opendir(src);

  if(dir == NULL){
    perror("error opening dir \n");
    return -1;
  }


  struct dirent *file;
  errno = 0;

  int dfd = dirfd(dir);
  if (dfd == -1) {
        perror("dirfd failed");
        closedir(dir);
        return -1;
    }

  while((file = readdir(dir)) != NULL){
    if(strcmp(file->d_name,".") == 0 || strcmp(file->d_name,"..") == 0){continue;} 

    int snp;
    int is_folder = 0;
    int is_file = 0;

    snp = aux_snprintf(dest, MAX_PATH_SIZE, file->d_name);

    if(snp == -1){
      closedir(dir);
      return -1;
    }

    snp = aux_snprintf(src, MAX_PATH_SIZE, file->d_name);

    if(snp == -1){
      closedir(dir);
      return -1;
    }


    if(file->d_type == DT_UNKNOWN){
      struct stat st;

      if (fstatat(dfd, file->d_name, &st, 0) == -1) {
          perror("fstatat");
          src[size_src] = '\0';
          dest[size_dest] = '\0';
          continue;
      }

      if (S_ISDIR(st.st_mode)) {
          is_folder = 1;
      }
      else if (S_ISREG(st.st_mode)) {
          is_file = 1;
      }
      
    }

    if(file->d_type == DT_DIR || is_folder == 1){ // if we are currently working with a folder

      snp = aux_mkdir(dest);

      if(snp == -1){
      closedir(dir);
      return -1;
    }

      snp = open_dir(src, dest);

      if(snp == -1){
      closedir(dir);
      return -1;
    }
    }

    else if(file->d_type == DT_REG || is_file == 1){

      int fd_src;
      int fd_dest;

      fd_src = open(src, O_RDONLY);
      
      if(fd_src == -1){
      closedir(dir);
      return -1;
    }

    fd_dest = open(dest, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if(fd_dest == -1){
      close(fd_src);
      closedir(dir);
      return -1;
    }
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while((bytes_read = read(fd_src, buffer, BUFFER_SIZE)) > 0){
      
      size_t total_written = 0;

       while (total_written < (size_t)bytes_read) {
        ssize_t bytes_written = write(fd_dest, buffer + total_written, (size_t)bytes_read - total_written);      
      if(bytes_written == -1){
        fprintf(stderr, "write error\n");
        close(fd_src);
        close(fd_dest);
        closedir(dir);
        return -1;
      }
      total_written += (size_t)bytes_written;
    }

      
    }
    if(bytes_read == -1){
      fprintf(stderr, "read error\n");
      close(fd_src);
      close(fd_dest);
      closedir(dir);
      return -1;
    }
      close(fd_src);
      close(fd_dest);
  }


  src[size_src] = '\0';
  dest[size_dest] = '\0';
  
  }

  closedir(dir);
  return 0;
}


int create_dir(const char *res_id, const char *vm_id, const char *vm_folder){

  int snp;
  int check;

  // it starts with "" bc aux_snprintf checks size and first iteration size needs to equal 0
  char dest[MAX_PATH_SIZE] = ""; 
  const char *message  = "tmp/CloudIST";

  snp = aux_snprintf(dest, sizeof(dest), message);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(dest);

  if(check == -1){
    return -1;
  }

  //folder res_id
  snp = aux_snprintf(dest, sizeof(dest), res_id);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(dest);

  if(check == -1){
    return -1;
  }

  //folder vm_id
  snp = aux_snprintf(dest, sizeof(dest), vm_id);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(dest);

  if(check == -1){
    return -1;
  }

  // full folder is now created, /tmp/CloudIST/<ID-reserva>/<ID-VM>, now we open vm_folder and copy to /tmp/...
  // src is the path to the vm_folder
  char src[MAX_PATH_SIZE];

  snp = snprintf(src, sizeof(src), "%s", vm_folder);
  
  if(snp < 0){
    fprintf(stderr, "snprintf error\n");
    return -1;}


  else if((size_t)snp >= sizeof(src)){
    fprintf(stderr, "snprintf truncated\n");
    return -1;
  }


  check = open_dir(src, dest);

  if(check == -1){
    return -1;
  }


  return 0;
}
