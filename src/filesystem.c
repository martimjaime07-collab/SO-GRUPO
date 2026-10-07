#define _XOPEN_SOURCE 700

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


int open_dir(char *buffer, char *vm_folder){

  
}


int create_dir(const char *res_id, const char *vm_id, const char *vm_folder){

  int snp;
  int check;

  // it starts with "" bc aux_snprintf checks size and first iteration size needs to equal 0
  char buffer[MAX_PATH_SIZE] = ""; 
  const char *message  = "tmp/CloudIST";

  snp = aux_snprintf(buffer, sizeof(buffer), message);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(buffer);

  if(check == -1){
    return -1;
  }

  //folder res_id
  snp = aux_snprintf(buffer, sizeof(buffer), res_id);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(buffer);

  if(check == -1){
    return -1;
  }

  //folder vm_id
  snp = aux_snprintf(buffer, sizeof(buffer), vm_id);

  if(snp == -1){
    return -1;
  }

  check = aux_mkdir(buffer);

  if(check == -1){
    return -1;
  }

  //full folder is now created, /tmp/CloudIST/<ID-reserva>/<ID-VM>, now we open vm_folder and copy to /tmp/...

  int check = 


  return 0;
}
