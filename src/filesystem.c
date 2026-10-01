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

  }


  closedir(dir);
  return count;
}
