#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <dirent.h>
#include <sys/sysinfo.h>
#include <stdbool.h>

pthread_cond_t end_char_counters;
pthread_cond_t end_counter;
pthread_mutex_t access_counter_data;
pthread_mutex_t access_filepaths;

sem_t write_counter_data;
sem_t read_counter_data;
sem_t write_path;
sem_t read_path;

int path_pointer;
int counter_data_pointer;
bool run_counter;
bool run_characters_counter;

char** filepaths_buffer;
int filepaths_num;
int counter_data_num;
int threads_num;

typedef struct {
    int characters;
    int lines;
} CounterData;

CounterData* counter_data;

typedef struct {
    char* directoryPath;
    char** extensions;
    int extNum;
    int level;
} DirectoryInfo;

const char* get_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if(!dot || dot == filename)
        return "";

    return dot + 1;
}

void create_full_path(const char* path, const char* filename, char* fullpath) {
    strcpy(fullpath, path);
    strcat(fullpath, "/");
    strcat(fullpath, filename);
}

void* counter(void* arg) {
    int lines_sum = 0;
    int characters_sum = 0;

    while (run_counter) {
        sem_wait(&read_counter_data);
        if(!run_counter){
            break;
        }

        pthread_mutex_lock(&access_counter_data);
        counter_data_pointer--;
        characters_sum += counter_data[counter_data_pointer].characters;
        lines_sum += counter_data[counter_data_pointer].lines;

        printf("Number of lines: %d\n", lines_sum);
        printf("Number of characters: %d\n", characters_sum);

        if(counter_data_pointer == 0) {
            pthread_cond_signal(&end_counter);
        }
        pthread_mutex_unlock(&access_counter_data);
        sem_post(&write_counter_data);
    }

    pthread_exit(NULL);
}

void* count_characters(void* arg) {
    while(run_characters_counter) {
        char filepath[PATH_MAX];
        sem_wait(&read_path);

        if(!run_characters_counter) {
            break;
        }
        
        pthread_mutex_lock(&access_filepaths);
        path_pointer--;
        strcpy(filepath, filepaths_buffer[path_pointer]);
        if(path_pointer == 0) {
            pthread_cond_signal(&end_char_counters);
        }
        pthread_mutex_unlock(&access_filepaths);
        sem_post(&write_path);
        
        int fd = open(filepath, O_RDONLY);
        if(fd == -1){
            fprintf(stderr, "Error in file opening %s: %s\n", filepath, strerror(errno));
            continue;
        }

        struct stat sb;
        fstat(fd, &sb);

        char* ptr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

        if(ptr == MAP_FAILED) {
            fprintf(stderr, "Error in mmap %s: %s\n", filepath, strerror(errno));
            continue;
        }

        int characters = 0;
        int lines = 0;

        for(int i = 0; i < sb.st_size; i++) {
            if(ptr[i] == '\n') {
                lines++;
            }
            else if(!isspace(ptr[i])) {
                characters++;
            }    
        }

        munmap(ptr, sb.st_size);
        close(fd);

        sem_wait(&write_counter_data);
        pthread_mutex_lock(&access_counter_data);
        counter_data[counter_data_pointer].characters = characters;
        counter_data[counter_data_pointer].lines = lines;
        counter_data_pointer++;
        pthread_mutex_unlock(&access_counter_data);
        sem_post(&read_counter_data);
    }

    pthread_exit(NULL);
}

void close_counters() {
    pthread_mutex_lock(&access_filepaths);
    while (path_pointer > 0) {
        pthread_cond_wait(&end_char_counters, &access_filepaths);
    }
    pthread_mutex_unlock(&access_filepaths);
        
    run_characters_counter = false;
    for(int i = 0; i < threads_num; i++) {
        sem_post(&read_path);
    }
}

void* file_finder(void* arg) {
    DirectoryInfo* dir_info = (DirectoryInfo*) arg;
    char* directory_path = dir_info->directoryPath;
    char** extensions = dir_info->extensions;
    int ext_num = dir_info->extNum;

    struct dirent* entry;
    DIR* dir = opendir(directory_path);

    if(dir == NULL) {
        fprintf(stderr, "Error in opening the file %s: %s\n", directory_path, strerror(errno));
        close_counters();
        pthread_exit(NULL);
    }

    while ((entry = readdir(dir)) != NULL) {
        if(entry->d_type == DT_REG) {
            bool find = false;
            char fullpath[PATH_MAX];
            const char* ext = get_extension(entry->d_name);
            for(int i = 0; i < ext_num; i++) {
                if(!strcmp(ext, extensions[i])) {
                    create_full_path(directory_path, entry->d_name, fullpath);
                    find = true;
                    break;
                }
            }

            if(find) {
                sem_wait(&write_path);
                pthread_mutex_lock(&access_filepaths);
                strcpy(filepaths_buffer[path_pointer], fullpath);
                path_pointer++;
                pthread_mutex_unlock(&access_filepaths);
                sem_post(&read_path);
            }
        }
    }

    rewinddir(dir);
    
    while ((entry = readdir(dir)) != NULL) {
        if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        else if(entry->d_type == DT_DIR) {
            char fullpath[PATH_MAX];
            create_full_path(directory_path, entry->d_name, fullpath);
            DirectoryInfo new_dir_info = {fullpath, extensions, ext_num, dir_info->level + 1};
            file_finder((void*) &new_dir_info);
        }
    }
    
    closedir(dir);

    if(dir_info->level == 0) {
        close_counters();
        pthread_exit(NULL);
    } 
}

int main(int argc, char* argv[]) {
    if(argc < 3) {
        perror("Not enough arguments");
        exit(1);
    }

    char* dir_path = argv[1];
    int ext_num = argc - 2;
    char** extensions = (char**) malloc(ext_num * sizeof(char*));
    for(int i = 0; i < ext_num; i++) {
        extensions[i] = (char*) malloc((strlen(argv[i + 2]) + 1) * sizeof(char));
        if(extensions[i]) {
            strcpy(extensions[i], argv[i + 2]);
        }
    }

    pthread_t* threads;
    pthread_t file_finder_th, counter_th;
    DirectoryInfo dirInfo = {dir_path, extensions, ext_num, 0};
    
    pthread_cond_init(&end_counter, NULL);
    pthread_cond_init(&end_char_counters, NULL);
    pthread_mutex_init(&access_counter_data, NULL);
    pthread_mutex_init(&access_filepaths, NULL);

    threads_num = get_nprocs();
    filepaths_num = 8;
    counter_data_num = 8;
    path_pointer = 0;
    counter_data_pointer = 0;

    run_counter = true;
    run_characters_counter = true;

    sem_init(&write_counter_data, 0, counter_data_num);
    sem_init(&read_counter_data, 0, 0);
    sem_init(&write_path, 0, filepaths_num);
    sem_init(&read_path, 0, 0);

    counter_data = (CounterData*) malloc(counter_data_num * sizeof(CounterData));
    threads = malloc(threads_num * sizeof(pthread_t));
    filepaths_buffer = (char**) malloc(filepaths_num * sizeof(char*));
    for(int i = 0; i < filepaths_num; i++) {
        filepaths_buffer[i] = (char*) malloc(PATH_MAX * sizeof(char));
    }
   
    pthread_create(&counter_th, NULL, counter, NULL);
    for(int i = 0; i < threads_num; i++) {
        pthread_create(&threads[i], NULL, count_characters, NULL);
    }
    pthread_create(&file_finder_th, NULL, file_finder, (void*) &dirInfo);
    
    pthread_join(file_finder_th, NULL);
    for(int i = 0; i < threads_num; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_lock(&access_counter_data);
    while (counter_data_pointer > 0) {
        pthread_cond_wait(&end_counter, &access_counter_data);
    }
    pthread_mutex_unlock(&access_counter_data);
    run_counter = false;
    sem_post(&read_counter_data);
    pthread_join(counter_th, NULL);
    
    free(threads);
    free(counter_data);
    for(int i = 0; i < ext_num; i++){
        free(extensions[i]);
    }
    free(extensions);

    for(int i = 0; i < filepaths_num; i++) {
        free(filepaths_buffer[i]);
    }
    free(filepaths_buffer);

    pthread_mutex_destroy(&access_counter_data);
    pthread_mutex_destroy(&access_filepaths);
    pthread_cond_destroy(&end_char_counters);
    pthread_cond_destroy(&end_counter);

    sem_destroy(&read_path);
    sem_destroy(&write_path);
    sem_destroy(&read_counter_data);
    sem_destroy(&write_counter_data);

    return 0;
}