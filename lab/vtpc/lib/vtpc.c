#define _GNU_SOURCE
#include "vtpc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BLOCK_SIZE 4096
#define CACHE_SIZE_BLOCKS 1024  // Кэш 4 МБ

typedef struct {
  char* data;
  int fd;
  off_t block_index;
  int dirty;
  unsigned long long frequency;
  int valid;
} CacheBlock;

typedef struct {
  int os_fd;
  off_t current_offset;
  off_t file_size;
  int flags;
} FileContext;

static CacheBlock cache[CACHE_SIZE_BLOCKS];
static int cache_initialized = 0;

#define MAX_OPEN_FILES 128
static FileContext open_files[MAX_OPEN_FILES];

static int init_cache() {
  if (cache_initialized)
    return 0;
  for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
    if (posix_memalign((void**)&cache[i].data, BLOCK_SIZE, BLOCK_SIZE) != 0) {
      errno = ENOMEM;
      return -1;
    }
    cache[i].valid = 0;
    cache[i].dirty = 0;
    cache[i].frequency = 0;
    cache[i].fd = -1;
  }
  for (int i = 0; i < MAX_OPEN_FILES; i++) {
    open_files[i].os_fd = -1;
  }
  cache_initialized = 1;
  return 0;
}

static int find_cache_block(int fd, off_t block_index) {
  for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
    if (cache[i].valid && cache[i].fd == fd &&
        cache[i].block_index == block_index) {
      return i;
    }
  }
  return -1;
}

static int evict_block() {
  int candidate = -1;
  unsigned long long max_freq = 0;
  int first_invalid = -1;

  for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
    if (!cache[i].valid) {
      if (first_invalid == -1)
        first_invalid = i;
      return i;
    }

    if (candidate == -1 || cache[i].frequency > max_freq) {
      max_freq = cache[i].frequency;
      candidate = i;
    }
  }

  if (candidate != -1) {
    if (cache[candidate].dirty) {
      off_t offset = cache[candidate].block_index * BLOCK_SIZE;
      lseek(cache[candidate].fd, offset, SEEK_SET);
      ssize_t written =
          write(cache[candidate].fd, cache[candidate].data, BLOCK_SIZE);
      if (written != BLOCK_SIZE) {
        perror("vtpc: eviction write failed");
      }
    }
    cache[candidate].valid = 0;
    cache[candidate].dirty = 0;
    cache[candidate].frequency = 0;
    return candidate;
  }

  return 0;
}

int vtpc_open(const char* path, int flags, int mode) {
  if (!cache_initialized) {
    if (init_cache() == -1)
      return -1;
  }

  int handle = -1;
  for (int i = 0; i < MAX_OPEN_FILES; i++) {
    if (open_files[i].os_fd == -1) {
      handle = i;
      break;
    }
  }
  if (handle == -1) {
    errno = EMFILE;
    return -1;
  }

  int os_fd = open(path, flags | O_DIRECT, mode);
  if (os_fd == -1) {
    return -1;
  }

  struct stat st;
  if (fstat(os_fd, &st) == -1) {
    close(os_fd);
    return -1;
  }

  open_files[handle].os_fd = os_fd;
  open_files[handle].current_offset = 0;
  open_files[handle].file_size = st.st_size;
  open_files[handle].flags = flags;

  return handle;
}

int vtpc_close(int fd) {
  if (fd < 0 || fd >= MAX_OPEN_FILES || open_files[fd].os_fd == -1) {
    errno = EBADF;
    return -1;
  }

  int os_fd = open_files[fd].os_fd;

  vtpc_fsync(fd);

  for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
    if (cache[i].valid && cache[i].fd == os_fd) {
      cache[i].valid = 0;
      cache[i].dirty = 0;
    }
  }

  int res = close(os_fd);
  open_files[fd].os_fd = -1;
  return res;
}

ssize_t vtpc_read(int fd, void* buf, size_t count) {
  if (fd < 0 || fd >= MAX_OPEN_FILES || open_files[fd].os_fd == -1) {
    errno = EBADF;
    return -1;
  }

  int os_fd = open_files[fd].os_fd;
  off_t offset = open_files[fd].current_offset;
  off_t file_size = open_files[fd].file_size;

  if (offset >= file_size) {
    return 0;
  }
  if (offset + count > file_size) {
    count = file_size - offset;
  }

  size_t bytes_read = 0;
  char* user_buf = (char*)buf;

  while (bytes_read < count) {
    off_t block_idx = (offset + bytes_read) / BLOCK_SIZE;
    size_t offset_in_block = (offset + bytes_read) % BLOCK_SIZE;
    size_t to_copy = BLOCK_SIZE - offset_in_block;
    if (to_copy > count - bytes_read)
      to_copy = count - bytes_read;

    int cache_idx = find_cache_block(os_fd, block_idx);

    if (cache_idx == -1) {
      cache_idx = evict_block();

      off_t disk_offset = block_idx * BLOCK_SIZE;
      ssize_t r = pread(os_fd, cache[cache_idx].data, BLOCK_SIZE, disk_offset);
      if (r == -1) {
        return -1;
      }
      if (r < BLOCK_SIZE) {
        memset(cache[cache_idx].data + r, 0, BLOCK_SIZE - r);
      }

      cache[cache_idx].valid = 1;
      cache[cache_idx].fd = os_fd;
      cache[cache_idx].block_index = block_idx;
      cache[cache_idx].dirty = 0;
      cache[cache_idx].frequency = 1;
    } else {
      cache[cache_idx].frequency++;
    }

    memcpy(
        user_buf + bytes_read, cache[cache_idx].data + offset_in_block, to_copy
    );
    bytes_read += to_copy;
  }

  open_files[fd].current_offset += bytes_read;
  return bytes_read;
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
  if (fd < 0 || fd >= MAX_OPEN_FILES || open_files[fd].os_fd == -1) {
    errno = EBADF;
    return -1;
  }

  int os_fd = open_files[fd].os_fd;
  off_t offset = open_files[fd].current_offset;
  size_t bytes_written = 0;
  const char* user_buf = (const char*)buf;

  while (bytes_written < count) {
    off_t block_idx = (offset + bytes_written) / BLOCK_SIZE;
    size_t offset_in_block = (offset + bytes_written) % BLOCK_SIZE;
    size_t to_copy = BLOCK_SIZE - offset_in_block;
    if (to_copy > count - bytes_written)
      to_copy = count - bytes_written;

    int cache_idx = find_cache_block(os_fd, block_idx);

    if (cache_idx == -1) {
      cache_idx = evict_block();

      if (to_copy < BLOCK_SIZE) {
        ssize_t r = pread(
            os_fd, cache[cache_idx].data, BLOCK_SIZE, block_idx * BLOCK_SIZE
        );
        if (r == -1) {
          memset(cache[cache_idx].data, 0, BLOCK_SIZE);
        } else if (r < BLOCK_SIZE) {
          memset(cache[cache_idx].data + r, 0, BLOCK_SIZE - r);
        }
      } else {
        // gg
      }

      cache[cache_idx].valid = 1;
      cache[cache_idx].fd = os_fd;
      cache[cache_idx].block_index = block_idx;
      cache[cache_idx].dirty = 0;
      cache[cache_idx].frequency = 1;
    } else {
      cache[cache_idx].frequency++;
    }

    memcpy(
        cache[cache_idx].data + offset_in_block,
        user_buf + bytes_written,
        to_copy
    );
    cache[cache_idx].dirty = 1;
    bytes_written += to_copy;
  }

  open_files[fd].current_offset += bytes_written;
  if (open_files[fd].current_offset > open_files[fd].file_size) {
    open_files[fd].file_size = open_files[fd].current_offset;
  }
  return bytes_written;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  if (fd < 0 || fd >= MAX_OPEN_FILES || open_files[fd].os_fd == -1) {
    errno = EBADF;
    return -1;
  }

  off_t new_offset = open_files[fd].current_offset;
  if (whence == SEEK_SET) {
    new_offset = offset;
  } else if (whence == SEEK_CUR) {
    new_offset += offset;
  } else if (whence == SEEK_END) {
    new_offset = open_files[fd].file_size + offset;
  } else {
    errno = EINVAL;
    return -1;
  }

  if (new_offset < 0) {
    errno = EINVAL;
    return -1;
  }

  open_files[fd].current_offset = new_offset;
  return new_offset;
}

int vtpc_fsync(int fd) {
  if (fd < 0 || fd >= MAX_OPEN_FILES || open_files[fd].os_fd == -1) {
    errno = EBADF;
    return -1;
  }

  int os_fd = open_files[fd].os_fd;

  for (int i = 0; i < CACHE_SIZE_BLOCKS; i++) {
    if (cache[i].valid && cache[i].fd == os_fd && cache[i].dirty) {
      off_t offset = cache[i].block_index * BLOCK_SIZE;
      ssize_t r = pwrite(os_fd, cache[i].data, BLOCK_SIZE, offset);
      if (r == -1)
        return -1;
      cache[i].dirty = 0;
    }
  }

  int res = fsync(os_fd);
  if (res == -1)
    return -1;

  if (ftruncate(os_fd, open_files[fd].file_size) == -1) {
    // gg
  }

  return res;
}
