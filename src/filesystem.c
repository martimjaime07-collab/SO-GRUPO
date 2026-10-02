#define _XOPEN_SOURCE 700

#include "filesystem.h"

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

static int compare_strings(const void *a, const void *b){
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa,sb);
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

ssize_t list_conf_files(const char *path, char ***buffer){

  *buffer = NULL; 

  DIR *dir = opendir(path);
  

  if(dir == NULL){
    perror("error opening dir");
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

    errno = 0;
    

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
        perror("realloc error");
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
      perror("strdup error");
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



  qsort(*buffer, count, sizeof(char *), compare_strings);
  
  if(readdir_errno == 0){
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