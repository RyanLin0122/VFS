#include <vfs.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <iostream>
#include <io.h>
#include <string.h>
#include <stdbool.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <cctype>
#include <intrin.h>
#include <climits>

const int VFS_IIO_MAGIC = 0x324F4941;

const int VFS_FNM_PATHNAME = 0x01;
const int VFS_FNM_NOESCAPE = 0x02;
const int VFS_FNM_PERIOD = 0x04;

#define VFS_FNM_NOSORT (0x04)
#define VFS_FNM_DOOFFS (0x08)
#define VFS_FNM_NOCHECK (0x10)
#define VFS_FNM_APPEND (0x20)

const int VFS_OPEN_ACCESS_MODE_MASK = 0x03;
const int VFS_OPEN_READ_ONLY = 0x00;
const int VFS_OPEN_WRITE_ONLY = 0x01;
const int VFS_OPEN_READ_WRITE = 0x02;

const int VFS_OPEN_APPEND = 0x08;
const int VFS_OPEN_TRUNCATE = 0x200;

const int VFS_DEFAULT_FILE_MODE = 0x0302;

static_assert(sizeof(VfsTrieNode) == 20, "TrieNode 必須剛好 20 bytes");

int vfs_iio_BLOCK_SIZEv = 4096;
int vfs_iio_CLOCK = 0;
int vfs_iio_IOMODE = 0;
int vfs_iio_CACHE_PAGES = 64;

int vfs_data_IOMODE = 0;
int vfs_data_CACHE_BYTES = 0x10000;

char vfs_glob_key_buffer[4096];

int vfs_errno = VFS_ERR_NONE;

long long vfs_stat_fat_read = 0, vfs_stat_fat_write = 0, vfs_stat_fat_scan_steps = 0;
long long vfs_stat_data_slide = 0, vfs_stat_data_read = 0, vfs_stat_data_write = 0;

DWORD get_page_size(void) {
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	return systemInfo.dwPageSize;
}

int file_exists(const char* fileName) {
	FILE* file = fopen(fileName, "rb");
	if (file) {
		fclose(file);
		return 1;
	}
	return 0;
}

static int vfs_file_seek64(FILE* fp, long long offset) {
#if defined(_WIN32)
	return _fseeki64(fp, offset, SEEK_SET);
#else
	return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
}

int bit_get(const char* byteValue, int bitIndex) {
	if (bitIndex >= 0) {
		return (byteValue[bitIndex >> 3] >> (bitIndex & 7)) & 1;
	}
	return 2;
}

static inline int find_first_set_bit(unsigned char n) {
	if (n == 0) return 0;
#if defined(_MSC_VER)
	unsigned long index;
	_BitScanForward(&index, n);
	return index;
#elif defined(__GNUC__) || defined(__clang__)
	return __builtin_ffs(n) - 1;
#else
	int count = 0;
	while ((n & 1) == 0) {
		n >>= 1;
		count++;
	}
	return count;
#endif
}

int bitfirst_different(const char* str1, const char* str2) {
	int byteIndex = 0;
	while (str1[byteIndex] == str2[byteIndex]) {
		if (str1[byteIndex] == '\0') {
			break;
		}
		byteIndex++;
	}

	int local_bit_offset = 0;

	if (bit_get(&str1[byteIndex], local_bit_offset) != bit_get(&str2[byteIndex], local_bit_offset)) {
		return byteIndex * 8 + local_bit_offset;
	}

	do {
		local_bit_offset++;
	} while (bit_get(&str1[byteIndex], local_bit_offset) == bit_get(&str2[byteIndex], local_bit_offset));

	return byteIndex * 8 + local_bit_offset;
}

int nblocks(int size) {
	if (size == 0) {
		return 1;
	}
	return (size - 1) / 512 + 1;
}

int blockno(int offset) {
	return offset / 512;
}

int lock_remove(const char* baseName) {
	char fileName[256];
	strcpy(fileName, baseName);
	strcat(fileName, ".lock");
	return remove(fileName);
}

int lock_check(const char* baseName, int accessMode) {
	char fileName[256];
	strcpy(fileName, baseName);
	strcat(fileName, ".lock");

	FILE* lockFile = fopen(fileName, "rb");
	if (!lockFile) {
		return 1;
	}

	char lockFlags[4];
	if (fread(lockFlags, 1, sizeof(lockFlags), lockFile) != sizeof(lockFlags)) {
		fclose(lockFile);
		return 0;
	}
	fclose(lockFile);

	if ((lockFlags[0] & 0x02) || !(lockFlags[0] & 0x01)) {
		return 0;
	}
	else {
		return (accessMode == 1);
	}
}

int lock_leave(const char* baseName) {
	char fileName[256];
	strcpy(fileName, baseName);
	strcat(fileName, ".lock");

	FILE* lockFile = fopen(fileName, "r+b");
	if (!lockFile) {
		return -1;
	}

	char lockFlags[4];
	int lockCount = 0;

	if (fread(lockFlags, 1, sizeof(lockFlags), lockFile) != sizeof(lockFlags)) {
		fclose(lockFile);
		return -1;
	}

	if (fread(&lockCount, sizeof(int), 1, lockFile) != 1) {
		fclose(lockFile);
		return -1;
	}

	lockCount--;

	if (fseek(lockFile, sizeof(lockFlags), SEEK_SET) != 0) {
		fclose(lockFile);
		return -1;
	}

	if (fwrite(&lockCount, sizeof(int), 1, lockFile) != 1) {
		fclose(lockFile);
		return -1;
	}

	fclose(lockFile);

	if (lockCount > 0) {
		return 0;
	}
	else {
		return lock_remove(baseName);
	}
}

void vfs_perror(void* unusedHandle, const char* prefixMessage) {
	const char* errorMessage = nullptr;

	switch (static_cast<char>(vfs_errno)) {
	case VFS_ERR_IIO_FILE: errorMessage = "Could not open/create the IIO file"; break;
	case VFS_ERR_DATA_FILE: errorMessage = "Could not open/create the data file"; break;
	case VFS_ERR_FAT_INIT: errorMessage = "Could not initialize the FAT channel"; break;
	case VFS_ERR_NT_INIT: errorMessage = "Could not initialize the NT channel"; break;
	case VFS_ERR_DT_INIT: errorMessage = "Could not initialize the DT channel"; break;
	case VFS_ERR_FAT_ALLOC: errorMessage = "Could not allocate the FAT channel"; break;
	case VFS_ERR_NT_ALLOC: errorMessage = "Could not allocate the NT channel"; break;
	case VFS_ERR_DT_ALLOC: errorMessage = "Could not allocate the DT channel"; break;
	case VFS_ERR_INVALID_HANDLE: errorMessage = "Invalid file system handle"; break;
	case VFS_ERR_FILE_CLOSED: errorMessage = "File already closed"; break;
	case VFS_ERR_FILE_NOT_FOUND: errorMessage = "File not found"; break;
	case VFS_ERR_NO_OPEN_FILES: errorMessage = "No files open"; break;
	case VFS_ERR_INVALID_FD: errorMessage = "Invalid file descriptor"; break;
	case VFS_ERR_INVALID_NODE: errorMessage = "Invalid node in NT"; break;
	case VFS_ERR_NO_MEMORY: errorMessage = "No memory available for opening files"; break;
	case VFS_ERR_LOCKED: errorMessage = "File system is locked by another process"; break;
	case VFS_ERR_FILE_EXISTS: errorMessage = "The file you are trying to create already exists"; break;
	case VFS_ERR_DT_INTERNAL: errorMessage = "Internal error in DT"; break;
	case VFS_ERR_INVALID_PARAM: errorMessage = "Invalid parameters"; break;
	default: errorMessage = "Internal error"; break;
	}
	printf("%s: %s\n", prefixMessage, errorMessage);
}

void vfs_stat_reset(void) {
	vfs_stat_fat_read = vfs_stat_fat_write = vfs_stat_fat_scan_steps = 0;
	vfs_stat_data_slide = vfs_stat_data_read = vfs_stat_data_write = 0;
}

void* cache_page_get_buffer(VfsIioCachePage* page) {
	if (page) {
		return reinterpret_cast<void*>(page->buffer_ptr_and_syncflag_storage & ~static_cast<uintptr_t>(1));
	}
	return nullptr;
}

void cache_page_set_sync(VfsIioCachePage* page, int sync_status) {
	if (page) {
		if (sync_status & 1) {
			page->buffer_ptr_and_syncflag_storage |= static_cast<uintptr_t>(1);
		}
		else {
			page->buffer_ptr_and_syncflag_storage &= ~static_cast<uintptr_t>(1);
		}
	}
}

int cache_page_get_sync(VfsIioCachePage* page) {
	if (page) {
		return static_cast<int>(page->buffer_ptr_and_syncflag_storage & static_cast<uintptr_t>(1));
	}
	return 0;
}

int header_size(VfsIioFile* file) {
	if (file && (8 + (8 * file->num_channels)) > vfs_iio_BLOCK_SIZEv) {
		printf("internal error: iio header overflow. Too many iio channels or too small BLOCK_SIZEv.\n");
		_exit(-1);
	}
	return vfs_iio_BLOCK_SIZEv;
}

int read_absolute_block_n(VfsIioFile* file, int absolute_block_index, int num_blocks_to_read, void* buffer) {
	if (!file || !file->file_handle) {
		return -1;
	}
	if (!buffer) {
		return -1;
	}
	if (num_blocks_to_read <= 0) {
		return 0;
	}

	long offset = static_cast<long>(header_size(file)) + static_cast<long>(absolute_block_index) * vfs_iio_BLOCK_SIZEv;
	if (fseek(file->file_handle, offset, SEEK_SET) != 0) {
		return -1;
	}

	size_t total_bytes_to_read = static_cast<size_t>(num_blocks_to_read) * vfs_iio_BLOCK_SIZEv;
	size_t bytes_actually_read = fread(buffer, 1, total_bytes_to_read, file->file_handle);

	if (bytes_actually_read < total_bytes_to_read) {
		memset(static_cast<char*>(buffer) + bytes_actually_read, 0, total_bytes_to_read - bytes_actually_read);
	}
	return static_cast<int>(total_bytes_to_read);
}

int write_absolute_block_n(VfsIioFile* file, int absolute_block_index, int num_blocks_to_write, const void* buffer) {
	if (!file || !file->file_handle) {
		return -1;
	}
	if (!buffer) {
		return -1;
	}
	if (num_blocks_to_write <= 0) {
		return 0;
	}

	long offset = static_cast<long>(header_size(file)) + static_cast<long>(absolute_block_index) * vfs_iio_BLOCK_SIZEv;
	if (fseek(file->file_handle, offset, SEEK_SET) != 0) {
		return -1;
	}

	size_t total_bytes_to_write = static_cast<size_t>(num_blocks_to_write) * vfs_iio_BLOCK_SIZEv;
	fwrite(buffer, 1, total_bytes_to_write, file->file_handle);

	return static_cast<int>(total_bytes_to_write);
}

int vfs_iio_blocks_per_chunk(VfsIioFile* file) {
	vfs_iio_CLOCK++;
	if (!file) {
		return -1;
	}

	int total_blocks_in_chunk = 0;
	if (file->channels) {
		for (short i = 0; i < file->num_channels; ++i) {
			if (file->channels[i]) {
				total_blocks_in_chunk += file->channels[i]->blocks_per_stripe;
			}
		}
	}
	return total_blocks_in_chunk;
}

int channel_block_to_absolute_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels) {
		return -1;
	}
	VfsIioChannel* target_channel = file->channels[channel_idx];
	if (!target_channel || target_channel->blocks_per_stripe == 0) {
		return -1;
	}

	int chunk_index = channel_relative_block_idx / target_channel->blocks_per_stripe;
	int offset_within_stripefor_target_channel = channel_relative_block_idx % target_channel->blocks_per_stripe;

	int base_offset_in_chunk = 0;
	for (int i = 0; i < channel_idx; ++i) {
		if (file->channels[i]) {
			base_offset_in_chunk += file->channels[i]->blocks_per_stripe;
		}
	}

	int total_blocks_perfull_chunk = vfs_iio_blocks_per_chunk(file);
	if (total_blocks_perfull_chunk < 0) return -1;

	return (offset_within_stripefor_target_channel + base_offset_in_chunk) + (chunk_index * total_blocks_perfull_chunk);
}

int channel_pos_to_absolute_block(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos) {
	if (vfs_iio_BLOCK_SIZEv == 0) return -1;
	int channel_relative_block_idx = channel_relative_byte_pos / vfs_iio_BLOCK_SIZEv;
	return channel_block_to_absolute_block(file, channel_idx, channel_relative_block_idx);
}

VfsIioChannel* vfs_iio_get_channel(VfsIioFile* file, int channel_idx) {
	vfs_iio_CLOCK++;

	if (file && file->channels && channel_idx >= 0 && channel_idx < file->num_channels) {
		return file->channels[channel_idx];
	}
	return nullptr;
}

int vfs_iio_channel_size(VfsIioChannel* channel) {
	vfs_iio_CLOCK++;

	if (channel) {
		return channel->current_size_bytes;
	}
	return -1;
}

int vfs_iio_channel_blocks(VfsIioChannel* channel) {
	vfs_iio_CLOCK++;

	if (channel) {
		return channel->blocks_per_stripe;
	}
	return -1;
}

int cache_page_choose_best_to_reuse(VfsIioFile* file, int channel_idx, int exclude_page_idx) {
	int best_idx_to_reuse = 0;
	int current_page_scan_idx = 0;
	int min_last_access_time = 0x7FFFFFFF;

	if (!file || channel_idx < 0 || channel_idx >= file->num_channels || !file->channels[channel_idx]) {
		return 0;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel->cache_header || !channel->cache_header->pages) {
		return 0;
	}

	VfsIioCache* cache = channel->cache_header;
	int allocated_pages_count = cache->num_pages_allocated;

	if (allocated_pages_count > 0) {
		for (current_page_scan_idx = 0; current_page_scan_idx < allocated_pages_count; ++current_page_scan_idx) {
			VfsIioCachePage* currentPage = cache->pages[current_page_scan_idx];
			if (currentPage && currentPage->last_access_time < min_last_access_time &&
				current_page_scan_idx != exclude_page_idx) {
				min_last_access_time = currentPage->last_access_time;
				best_idx_to_reuse = current_page_scan_idx;
			}
		}
	}
	return best_idx_to_reuse;
}

int cache_expand(VfsIioFile* file, int channel_idx, int required_page_array_idx) {
	if (!file || channel_idx < 0 || channel_idx >= file->num_channels || !file->channels[channel_idx]) {
		return 0;
	}
	VfsIioChannel* channel = file->channels[channel_idx];

	int current_allocated_count = 0;
	if (channel->cache_header) {
		current_allocated_count = channel->cache_header->num_pages_allocated;
	}

	int initial_allocated_count = current_allocated_count;
	int new_allocated_count = current_allocated_count;

	if (new_allocated_count == 0) {
		new_allocated_count = 1;
	}

	while (new_allocated_count <= required_page_array_idx || new_allocated_count < 1024) {
		new_allocated_count *= 2;
		if (new_allocated_count == 0) {
			new_allocated_count = required_page_array_idx > 1024 ? required_page_array_idx + 1 : 1024;
			if (new_allocated_count < initial_allocated_count) new_allocated_count = initial_allocated_count * 2;
			if (new_allocated_count == 0) new_allocated_count = 2048;
			break;
		}
	}

	if (!channel->cache_header) {
		channel->cache_header = static_cast<VfsIioCache*>(malloc(sizeof(VfsIioCache)));
		if (!channel->cache_header) return 0;
		channel->cache_header->pages = nullptr;
		channel->cache_header->num_pages_allocated = 0;
		channel->cache_header->num_pages_active = 0;
	}

	channel->cache_header->num_pages_allocated = new_allocated_count;
	VfsIioCachePage** new_pages_array = static_cast<VfsIioCachePage**>(
		realloc(channel->cache_header->pages, sizeof(VfsIioCachePage*) * new_allocated_count));

	if (!new_pages_array) {
		channel->cache_header->num_pages_allocated = initial_allocated_count;
		return 0;
	}
	channel->cache_header->pages = new_pages_array;

	for (int i = initial_allocated_count; i < new_allocated_count; ++i) {
		channel->cache_header->pages[i] = nullptr;
	}

	return new_allocated_count;
}

int cache_page_dump(VfsIioFile* file, int channel_idx, int page_array_idx) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels ||
		!file->channels[channel_idx]) {
		return -1;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel->cache_header || !channel->cache_header->pages || page_array_idx < 0 ||
		page_array_idx >= channel->cache_header->num_pages_allocated) {
		return -1;
	}

	VfsIioCachePage* page_to_dump = channel->cache_header->pages[page_array_idx];
	if (!page_to_dump) {
		return -1;
	}

	int absolute_disk_block = channel_pos_to_absolute_block(file, channel_idx, page_to_dump->disk_block_position);
	if (absolute_disk_block < 0) return -1;

	void* page_buffer = cache_page_get_buffer(page_to_dump);
	if (!page_buffer) return -1;

	if (write_absolute_block_n(file, absolute_disk_block, channel->blocks_per_stripe, page_buffer) < 0) {
		return -1;
	}

	cache_page_set_sync(page_to_dump, 1);
	return 0;
}

void cache_pageflush(VfsIioFile* file, int channel_idx, int page_array_idx) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels ||
		!file->channels[channel_idx]) {
		return;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel->cache_header || !channel->cache_header->pages || page_array_idx < 0 ||
		page_array_idx >= channel->cache_header->num_pages_allocated) {
		return;
	}

	VfsIioCachePage* page_toflush = channel->cache_header->pages[page_array_idx];
	if (page_toflush && !cache_page_get_sync(page_toflush)) {
		cache_page_dump(file, channel_idx, page_array_idx);
	}
}

void cache_page_create(VfsIioFile* file, int channel_idx, int page_array_idx_to_create_at) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels ||
		!file->channels[channel_idx]) {
		return;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel->cache_header || !channel->cache_header->pages || page_array_idx_to_create_at < 0 ||
		page_array_idx_to_create_at >= channel->cache_header->num_pages_allocated) {
		return;
	}

	VfsIioCache* cache = channel->cache_header;

	if (cache->pages[page_array_idx_to_create_at] == nullptr) {
		if (cache->num_pages_active >= vfs_iio_CACHE_PAGES) {
			int idx_to_reuse = cache_page_choose_best_to_reuse(file, channel_idx, page_array_idx_to_create_at);
			cache_pageflush(file, channel_idx, idx_to_reuse);

			cache->pages[page_array_idx_to_create_at] = cache->pages[idx_to_reuse];
			cache->pages[idx_to_reuse] = nullptr;

			VfsIioCachePage* reused_page = cache->pages[page_array_idx_to_create_at];
			reused_page->disk_block_position = 0;
			reused_page->last_access_time = vfs_iio_CLOCK;
			cache_page_set_sync(reused_page, 1);
		}
		else {
			cache->num_pages_active++;
			VfsIioCachePage* new_page = static_cast<VfsIioCachePage*>(malloc(sizeof(VfsIioCachePage)));
			if (!new_page) {
				cache->num_pages_active--;
				return;
			}

			size_t buffer_size = static_cast<size_t>(vfs_iio_BLOCK_SIZEv) * channel->blocks_per_stripe;
			if (buffer_size == 0 && channel->blocks_per_stripe > 0) {
				free(new_page);
				cache->num_pages_active--;
				return;
			}
			if (buffer_size == 0 && channel->blocks_per_stripe == 0) {
			}

			void* buffer = nullptr;
			if (buffer_size > 0) {
				buffer = malloc(buffer_size);
				if (!buffer) {
					free(new_page);
					cache->num_pages_active--;
					return;
				}
			}

			new_page->buffer_ptr_and_syncflag_storage = reinterpret_cast<uintptr_t>(buffer);
			new_page->disk_block_position = 0;
			new_page->last_access_time = vfs_iio_CLOCK;
			cache_page_set_sync(new_page, 1);

			cache->pages[page_array_idx_to_create_at] = new_page;
		}
	}
}

int cache_page_refresh(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels ||
		!file->channels[channel_idx]) {
		return -1;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (channel->blocks_per_stripe == 0 || vfs_iio_BLOCK_SIZEv == 0) {
		if (channel->blocks_per_stripe == 0 && channel_relative_byte_pos == 0) return 0;
		return -1;
	}

	int page_size_bytes = vfs_iio_BLOCK_SIZEv * channel->blocks_per_stripe;
	int page_array_idx = channel_relative_byte_pos / page_size_bytes;

	if (!channel->cache_header || page_array_idx >= channel->cache_header->num_pages_allocated) {
		if (cache_expand(file, channel_idx, page_array_idx) <= page_array_idx) {
			return -1;
		}
	}
	VfsIioCache* cache = channel->cache_header;
	if (!cache || !cache->pages) return -1;

	if (cache->pages[page_array_idx] == nullptr) {
		cache_page_create(file, channel_idx, page_array_idx);
		if (cache->pages[page_array_idx] == nullptr) {
			return -1;
		}
	}

	VfsIioCachePage* target_page = cache->pages[page_array_idx];

	int aligned_page_start_pos = page_array_idx * page_size_bytes;

	int absolute_disk_block = channel_pos_to_absolute_block(file, channel_idx, aligned_page_start_pos);
	if (absolute_disk_block < 0) return -1;

	void* page_buffer = cache_page_get_buffer(target_page);
	if (!page_buffer && channel->blocks_per_stripe > 0) return -1;

	if (channel->blocks_per_stripe > 0) {
		if (read_absolute_block_n(file, absolute_disk_block, channel->blocks_per_stripe, page_buffer) < 0) {
			return -1;
		}
	}

	cache_page_set_sync(target_page, 1);
	target_page->disk_block_position = aligned_page_start_pos;
	target_page->last_access_time = vfs_iio_CLOCK;

	return 0;
}

int is_in_cache(VfsIioFile* file, int channel_idx, int channel_relative_block_idx) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels) {
		return 0;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel || !channel->cache_header || !channel->cache_header->pages || channel->blocks_per_stripe == 0) {
		return 0;
	}

	int page_array_idx = channel_relative_block_idx / channel->blocks_per_stripe;

	if (page_array_idx < 0 || page_array_idx >= channel->cache_header->num_pages_allocated) {
		return 0;
	}
	return (channel->cache_header->pages[page_array_idx] != nullptr);
}

void cache_update(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels) {
		return;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (!channel || (channel->blocks_per_stripe == 0 && channel_relative_byte_pos != 0) || vfs_iio_BLOCK_SIZEv == 0) {
		if (channel && channel->blocks_per_stripe == 0 && channel_relative_byte_pos == 0) {
		}
		else
			return;
	}

	int page_size_bytes = vfs_iio_BLOCK_SIZEv * channel->blocks_per_stripe;
	if (page_size_bytes == 0 && channel_relative_byte_pos != 0) return;

	int aligned_page_start_byte_pos = channel_relative_byte_pos;
	if (page_size_bytes > 0) {
		aligned_page_start_byte_pos = (channel_relative_byte_pos / page_size_bytes) * page_size_bytes;
	}

	int page_array_idx = 0;
	if (page_size_bytes > 0) {
		page_array_idx = aligned_page_start_byte_pos / page_size_bytes;
	}

	VfsIioCache* cache = channel->cache_header;
	if (!cache || page_array_idx >= cache->num_pages_allocated) {
		if (cache_expand(file, channel_idx, page_array_idx) <= page_array_idx && page_size_bytes > 0) {
			return;
		}
		cache = channel->cache_header;
		if (!cache) return;
	}

	if (!cache->pages || cache->pages[page_array_idx] == nullptr) {
		cache_page_create(file, channel_idx, page_array_idx);
		if (!cache->pages || cache->pages[page_array_idx] == nullptr) return;
	}

	cache_pageflush(file, channel_idx, page_array_idx);
	cache_page_refresh(file, channel_idx, aligned_page_start_byte_pos);
}

int cache_create(VfsIioFile* file, int channel_idx) {
	if (!file) {
		return -1;
	}
	return cache_page_refresh(file, channel_idx, 0);
}

int cache_destroy(VfsIioCache* cache_to_destroy) {
	if (!cache_to_destroy) {
		return -1;
	}

	if (cache_to_destroy->pages) {
		for (int i = 0; i < cache_to_destroy->num_pages_allocated; ++i) {
			VfsIioCachePage* page = cache_to_destroy->pages[i];
			if (page) {
				void* buffer = cache_page_get_buffer(page);
				if (buffer) {
					free(buffer);
				}
				free(page);
			}
		}
		free(cache_to_destroy->pages);
	}
	free(cache_to_destroy);
	return 0;
}

void cacheflush(VfsIioFile* file, int channel_idx) {
	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels) {
		return;
	}
	VfsIioChannel* channel = file->channels[channel_idx];
	if (channel && channel->cache_header && channel->cache_header->pages) {
		for (int i = 0; i < channel->cache_header->num_pages_allocated; ++i) {
			if (channel->cache_header->pages[i] != nullptr) {
				cache_pageflush(file, channel_idx, i);
			}
		}
	}
}

void flush_data(VfsIioFile* file) {
	if (!file) {
		return;
	}
	for (short i = 0; i < file->num_channels; ++i) {
		cacheflush(file, i);
	}
	if (file->file_handle) {
		fflush(file->file_handle);
	}
}

int cache_read_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx, void* buffer) {
	if (!file || !buffer || vfs_iio_BLOCK_SIZEv == 0) return -1;

	if (!is_in_cache(file, channel_idx, channel_relative_block_idx)) {
		cache_update(file, channel_idx, channel_relative_block_idx * vfs_iio_BLOCK_SIZEv);
	}

	VfsIioChannel* channel = vfs_iio_get_channel(file, channel_idx);
	if (!channel || !channel->cache_header || !channel->cache_header->pages || channel->blocks_per_stripe == 0) {
		return -1;
	}

	int page_array_idx = channel_relative_block_idx / channel->blocks_per_stripe;
	if (page_array_idx < 0 || page_array_idx >= channel->cache_header->num_pages_allocated) {
		return -1;
	}

	VfsIioCachePage* page = channel->cache_header->pages[page_array_idx];
	if (!page) {
		return -1;
	}

	void* page_buffer_base = cache_page_get_buffer(page);
	if (!page_buffer_base) return -1;

	int block_start_byte_in_channel = channel_relative_block_idx * vfs_iio_BLOCK_SIZEv;
	int block_offset_in_page_buffer = block_start_byte_in_channel - page->disk_block_position;

	if (block_offset_in_page_buffer < 0 ||
		(block_offset_in_page_buffer + vfs_iio_BLOCK_SIZEv) > (channel->blocks_per_stripe * vfs_iio_BLOCK_SIZEv)) {
		return -1;
	}

	memcpy(buffer, static_cast<char*>(page_buffer_base) + block_offset_in_page_buffer, vfs_iio_BLOCK_SIZEv);

	page->last_access_time = vfs_iio_CLOCK;
	return vfs_iio_BLOCK_SIZEv;
}

unsigned int cache_read_partial_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx,
	int offset_in_block, int end_offset_in_block, void* buffer) {
	if (!file || !buffer || offset_in_block < 0 || end_offset_in_block < offset_in_block ||
		end_offset_in_block >= vfs_iio_BLOCK_SIZEv) {
		return 0;
	}

	if (!is_in_cache(file, channel_idx, channel_relative_block_idx)) {
		cache_update(file, channel_idx, channel_relative_block_idx * vfs_iio_BLOCK_SIZEv);
	}

	VfsIioChannel* channel = vfs_iio_get_channel(file, channel_idx);
	if (!channel || !channel->cache_header || !channel->cache_header->pages || channel->blocks_per_stripe == 0) {
		return 0;
	}

	int page_array_idx = channel_relative_block_idx / channel->blocks_per_stripe;
	if (page_array_idx < 0 || page_array_idx >= channel->cache_header->num_pages_allocated) {
		return 0;
	}

	VfsIioCachePage* page = channel->cache_header->pages[page_array_idx];
	if (!page) {
		return 0;
	}

	void* page_buffer_base = cache_page_get_buffer(page);
	if (!page_buffer_base) return 0;

	int block_start_byte_in_channel = channel_relative_block_idx * vfs_iio_BLOCK_SIZEv;
	int block_offset_in_page_buffer = block_start_byte_in_channel - page->disk_block_position;

	char* source_ptr = static_cast<char*>(page_buffer_base) + block_offset_in_page_buffer + offset_in_block;
	unsigned int bytes_to_copy = end_offset_in_block - offset_in_block + 1;

	if (block_offset_in_page_buffer < 0 ||
		(block_offset_in_page_buffer + offset_in_block + bytes_to_copy) >
			(static_cast<unsigned int>(channel->blocks_per_stripe) * vfs_iio_BLOCK_SIZEv)) {
		return 0;
	}

	memcpy(buffer, source_ptr, bytes_to_copy);
	page->last_access_time = vfs_iio_CLOCK;
	return bytes_to_copy;
}

int cache_write_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx, const void* buffer) {
	if (!file || !buffer || vfs_iio_BLOCK_SIZEv == 0) return -1;

	if (!is_in_cache(file, channel_idx, channel_relative_block_idx)) {
		cache_update(file, channel_idx, channel_relative_block_idx * vfs_iio_BLOCK_SIZEv);
	}

	VfsIioChannel* channel = vfs_iio_get_channel(file, channel_idx);
	if (!channel || !channel->cache_header || !channel->cache_header->pages || channel->blocks_per_stripe == 0) {
		return -1;
	}

	int page_array_idx = channel_relative_block_idx / channel->blocks_per_stripe;
	if (page_array_idx < 0 || page_array_idx >= channel->cache_header->num_pages_allocated) {
		return -1;
	}

	VfsIioCachePage* page = channel->cache_header->pages[page_array_idx];
	if (!page) {
		return -1;
	}

	void* page_buffer_base = cache_page_get_buffer(page);
	if (!page_buffer_base) return -1;

	int block_start_byte_in_channel = channel_relative_block_idx * vfs_iio_BLOCK_SIZEv;
	int block_offset_in_page_buffer = block_start_byte_in_channel - page->disk_block_position;

	if (block_offset_in_page_buffer < 0 ||
		(block_offset_in_page_buffer + vfs_iio_BLOCK_SIZEv) > (channel->blocks_per_stripe * vfs_iio_BLOCK_SIZEv)) {
		return -1;
	}

	memcpy(static_cast<char*>(page_buffer_base) + block_offset_in_page_buffer, buffer, vfs_iio_BLOCK_SIZEv);

	cache_page_set_sync(page, 0);
	page->last_access_time = vfs_iio_CLOCK;
	return vfs_iio_BLOCK_SIZEv;
}

int cache_write_partial_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx,
	int offset_in_block, int end_offset_in_block, const void* buffer) {
	if (!file || !buffer || offset_in_block < 0 || end_offset_in_block < offset_in_block ||
		end_offset_in_block >= vfs_iio_BLOCK_SIZEv) {
		return 0;
	}

	if (!is_in_cache(file, channel_idx, channel_relative_block_idx)) {
		cache_update(file, channel_idx, channel_relative_block_idx * vfs_iio_BLOCK_SIZEv);
	}

	VfsIioChannel* channel = vfs_iio_get_channel(file, channel_idx);
	if (!channel || !channel->cache_header || !channel->cache_header->pages || channel->blocks_per_stripe == 0) {
		return 0;
	}

	int page_array_idx = channel_relative_block_idx / channel->blocks_per_stripe;
	if (page_array_idx < 0 || page_array_idx >= channel->cache_header->num_pages_allocated) {
		return 0;
	}

	VfsIioCachePage* page = channel->cache_header->pages[page_array_idx];
	if (!page) {
		return 0;
	}

	void* page_buffer_base = cache_page_get_buffer(page);
	if (!page_buffer_base) return 0;

	int block_start_byte_in_channel = channel_relative_block_idx * vfs_iio_BLOCK_SIZEv;
	int block_offset_in_page_buffer = block_start_byte_in_channel - page->disk_block_position;
	char* destination_ptr = static_cast<char*>(page_buffer_base) + block_offset_in_page_buffer + offset_in_block;

	int bytes_to_copy = end_offset_in_block - offset_in_block + 1;

	if (block_offset_in_page_buffer < 0 ||
		static_cast<unsigned int>(block_offset_in_page_buffer + offset_in_block + bytes_to_copy) >
			(static_cast<unsigned int>(channel->blocks_per_stripe) * vfs_iio_BLOCK_SIZEv)) {
		return 0;
	}

	memcpy(destination_ptr, buffer, bytes_to_copy);

	cache_page_set_sync(page, 0);
	page->last_access_time = vfs_iio_CLOCK;
	return bytes_to_copy;
}

int write_header(VfsIioFile* file) {
	if (!file || !file->file_handle) {
		return -1;
	}

	struct FileHeaderPart {
		int magic_number;
		int num_channels_int;
	} file_header;

	file_header.magic_number = VFS_IIO_MAGIC;
	file_header.num_channels_int = file->num_channels;

	if (fseek(file->file_handle, 0, SEEK_SET) != 0) {
		return -1;
	}
	if (fwrite(&file_header, 1, sizeof(FileHeaderPart), file->file_handle) != sizeof(FileHeaderPart)) {
		return -1;
	}

	struct ChannelHeaderPart {
		int blocks_per_stripe;
		int current_size_bytes;
	} channel_header;

	for (short i = 0; i < file->num_channels; ++i) {
		if (!file->channels || !file->channels[i]) {
			continue;
		}
		VfsIioChannel* current_channel = file->channels[i];
		channel_header.blocks_per_stripe = current_channel->blocks_per_stripe;
		channel_header.current_size_bytes = current_channel->current_size_bytes;

		if (fwrite(&channel_header, 1, sizeof(ChannelHeaderPart), file->file_handle) != sizeof(ChannelHeaderPart)) {
			return -1;
		}
	}

	if (fflush(file->file_handle) != 0) {
		return -1;
	}
	return 0;
}

int read_header(VfsIioFile* file) {
	if (!file || !file->file_handle) {
		return -1;
	}

	if (fseek(file->file_handle, 0, SEEK_SET) != 0) {
		return -1;
	}

	struct FileHeaderData {
		int magic_number;
		int num_channels_as_int;
	} header_data;

	if (fread(&header_data, sizeof(FileHeaderData), 1, file->file_handle) != 1) {
		return -1;
	}

	if (header_data.magic_number != VFS_IIO_MAGIC) {
		return -1;
	}

	file->num_channels = static_cast<short>(header_data.num_channels_as_int);
	if (file->num_channels < 0) return -1;

	if (file->num_channels > 0) {
		file->channels = static_cast<VfsIioChannel**>(malloc(sizeof(VfsIioChannel*) * file->num_channels));
		if (!file->channels) {
			file->num_channels = 0;
			return -1;
		}
		for (short i = 0; i < file->num_channels; ++i) {
			file->channels[i] = nullptr;
		}

		struct ChannelHeaderData {
			int blocks_per_stripe;
			int current_size_bytes;
		} channel_data;

		for (short i = 0; i < file->num_channels; ++i) {
			file->channels[i] = static_cast<VfsIioChannel*>(malloc(sizeof(VfsIioChannel)));
			if (!file->channels[i]) {
				for (short j = 0; j < i; ++j) free(file->channels[j]);
				free(file->channels);
				file->channels = nullptr;
				file->num_channels = 0;
				return -1;
			}

			if (fread(&channel_data, sizeof(ChannelHeaderData), 1, file->file_handle) != 1) {
				for (short j = 0; j <= i; ++j) free(file->channels[j]);
				free(file->channels);
				file->channels = nullptr;
				file->num_channels = 0;
				return -1;
			}
			file->channels[i]->blocks_per_stripe = channel_data.blocks_per_stripe;
			file->channels[i]->current_size_bytes = channel_data.current_size_bytes;
			file->channels[i]->current_seek_position = 0;
			file->channels[i]->cache_header = nullptr;
		}
	}
	else {
		file->channels = nullptr;
	}

	for (short i = 0; i < file->num_channels; ++i) {
		if (cache_create(file, i) != 0) {
			for (short j = 0; j < file->num_channels; ++j) {
				if (file->channels[j]) {
					if (file->channels[j]->cache_header) cache_destroy(file->channels[j]->cache_header);
					free(file->channels[j]);
				}
			}
			if (file->channels) free(file->channels);
			file->channels = nullptr;
			file->num_channels = 0;
			return -1;
		}
	}
	return 0;
}

void auto_truncate(VfsIioFile* file) {
	if (!file || !file->file_handle || vfs_iio_BLOCK_SIZEv <= 0) {
		return;
	}

	long current_header_size = header_size(file);
	if (current_header_size < 0) {
		return;
	}

	int total_blocks_in_one_full_chunk = 0;
	if (file->num_channels > 0) {
		total_blocks_in_one_full_chunk = vfs_iio_blocks_per_chunk(file);
	}

	int max_chunks_needed = 0;
	if (file->num_channels > 0 && file->channels) {
		for (short i = 0; i < file->num_channels; ++i) {
			VfsIioChannel* channel = file->channels[i];
			if (channel) {
				if (channel->blocks_per_stripe > 0) {
					int num_data_blocks_for_channel =
						(channel->current_size_bytes + vfs_iio_BLOCK_SIZEv - 1) / vfs_iio_BLOCK_SIZEv;
					int num_chunks_for_channel =
						(num_data_blocks_for_channel + channel->blocks_per_stripe - 1) / channel->blocks_per_stripe;
					if (num_chunks_for_channel > max_chunks_needed) {
						max_chunks_needed = num_chunks_for_channel;
					}
				}
				else if (channel->current_size_bytes > 0) {
					if (max_chunks_needed == 0) max_chunks_needed = 1;
				}
			}
		}
	}

	long new_file_size;
	if (max_chunks_needed > 0 && total_blocks_in_one_full_chunk > 0) {
		long total_data_blocks_to_keep = static_cast<long>(max_chunks_needed) * total_blocks_in_one_full_chunk;
		new_file_size = current_header_size + total_data_blocks_to_keep * vfs_iio_BLOCK_SIZEv;
	}
	else {
		new_file_size = current_header_size;
	}

	int crt_fd = _fileno(file->file_handle);
	if (crt_fd != -1) {
		HANDLE win_handle = reinterpret_cast<HANDLE>(_get_osfhandle(crt_fd));
		if (win_handle != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER li_new_file_size;
			li_new_file_size.QuadPart = new_file_size;

			if (SetFilePointerEx(win_handle, li_new_file_size, NULL, FILE_BEGIN)) {
				if (!SetEndOfFile(win_handle)) {
					DWORD error = GetLastError();
				}
			}
			else {
				DWORD error = GetLastError();
			}
		}
		else {
		}
	}
	else {
	}
}

int vfs_iio_seek(VfsIioFile* file, int channel_idx, int seek_position) {
	vfs_iio_CLOCK++;

	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels ||
		!file->channels[channel_idx]) {
		return -1;
	}

	file->channels[channel_idx]->current_seek_position = seek_position;
	return seek_position;
}

int vfs_iio_read(VfsIioFile* file, int channel_idx, void* buffer, int bytes_to_read) {
	vfs_iio_CLOCK++;
	if (!file || !file->file_handle || !buffer || bytes_to_read <= 0) {
		return -1;
	}
	if (channel_idx < 0 || channel_idx >= file->num_channels || !file->channels || !file->channels[channel_idx]) {
		return -1;
	}

	VfsIioChannel* channel = file->channels[channel_idx];
	int current_pos = channel->current_seek_position;

	if (current_pos >= channel->current_size_bytes) {
		if (channel->current_size_bytes == 0 && current_pos == 0) {
		}
		else {
			return 0;
		}
	}

	int readable_bytes = channel->current_size_bytes - current_pos;
	if (bytes_to_read > readable_bytes) {
		bytes_to_read = readable_bytes;
	}
	if (bytes_to_read <= 0) return 0;

	int total_bytes_read = 0;
	char* current_buffer_ptr = static_cast<char*>(buffer);
	int remaining_bytes_to_read = bytes_to_read;

	while (remaining_bytes_to_read > 0) {
		vfs_iio_CLOCK++;
		int current_block_idx = current_pos / vfs_iio_BLOCK_SIZEv;
		int offset_in_block = current_pos % vfs_iio_BLOCK_SIZEv;
		int bytesfrom_this_block = vfs_iio_BLOCK_SIZEv - offset_in_block;
		if (bytesfrom_this_block > remaining_bytes_to_read) {
			bytesfrom_this_block = remaining_bytes_to_read;
		}

		int read_count;
		if (offset_in_block == 0 && bytesfrom_this_block == vfs_iio_BLOCK_SIZEv) {
			read_count = cache_read_channel_block(file, channel_idx, current_block_idx, current_buffer_ptr);
		}
		else {
			read_count = cache_read_partial_channel_block(file, channel_idx, current_block_idx, offset_in_block,
				offset_in_block + bytesfrom_this_block - 1, current_buffer_ptr);
		}

		if (read_count <= 0 && bytesfrom_this_block > 0) {
			break;
		}
		if (read_count > 0) {
			current_buffer_ptr += read_count;
			total_bytes_read += read_count;
			current_pos += read_count;
			remaining_bytes_to_read -= read_count;
		}
		else {
			break;
		}
	}
	channel->current_seek_position = current_pos;
	return total_bytes_read;
}

int vfs_iio_write(VfsIioFile* file, int channel_idx, const void* buffer, int bytes_to_write) {
	vfs_iio_CLOCK++;
	if (!file || !file->file_handle || !buffer || bytes_to_write <= 0) {
		return -1;
	}
	if (channel_idx < 0 || channel_idx >= file->num_channels || !file->channels || !file->channels[channel_idx]) {
		return -1;
	}

	VfsIioChannel* channel = file->channels[channel_idx];
	int current_pos = channel->current_seek_position;

	int total_bytes_written = 0;
	const char* current_buffer_ptr = static_cast<const char*>(buffer);
	int remaining_bytes_to_write = bytes_to_write;

	while (remaining_bytes_to_write > 0) {
		vfs_iio_CLOCK++;
		int current_block_idx = current_pos / vfs_iio_BLOCK_SIZEv;
		int offset_in_block = current_pos % vfs_iio_BLOCK_SIZEv;
		int bytes_to_this_block = vfs_iio_BLOCK_SIZEv - offset_in_block;
		if (bytes_to_this_block > remaining_bytes_to_write) {
			bytes_to_this_block = remaining_bytes_to_write;
		}

		int written_count;
		if (offset_in_block == 0 && bytes_to_this_block == vfs_iio_BLOCK_SIZEv) {
			written_count = cache_write_channel_block(file, channel_idx, current_block_idx, current_buffer_ptr);
		}
		else {
			written_count = cache_write_partial_channel_block(file, channel_idx, current_block_idx, offset_in_block,
				offset_in_block + bytes_to_this_block - 1, current_buffer_ptr);
		}

		if (written_count <= 0 && bytes_to_this_block > 0) {
			break;
		}
		if (written_count > 0) {
			current_buffer_ptr += written_count;
			total_bytes_written += written_count;
			current_pos += written_count;
			remaining_bytes_to_write -= written_count;
		}
		else {
			break;
		}
	}

	channel->current_seek_position = current_pos;
	if (current_pos > channel->current_size_bytes) {
		channel->current_size_bytes = current_pos;
	}
	return total_bytes_written;
}

int vfs_iio_channel_truncate(VfsIioFile* file, int channel_idx) {
	vfs_iio_CLOCK++;

	if (!file || !file->channels || channel_idx < 0 || channel_idx >= file->num_channels) {
		return -1;
	}

	VfsIioChannel* channel_to_truncate = file->channels[channel_idx];
	if (!channel_to_truncate) {
		return -1;
	}

	channel_to_truncate->current_size_bytes = channel_to_truncate->current_seek_position;
	return 0;
}

int vfs_iio_allocate_channel(VfsIioFile* file, int blocks_per_stripefor_new_channel) {
	vfs_iio_CLOCK++;
	if (!file) {
		return -1;
	}

	VfsIioChannel* new_channel = static_cast<VfsIioChannel*>(malloc(sizeof(VfsIioChannel)));
	if (!new_channel) {
		return -1;
	}
	new_channel->blocks_per_stripe = blocks_per_stripefor_new_channel;
	new_channel->current_size_bytes = 0;
	new_channel->current_seek_position = 0;
	new_channel->cache_header = nullptr;

	short old_num_channels = file->num_channels;
	short new_num_channels = old_num_channels + 1;

	VfsIioChannel** new_channels_array =
		static_cast<VfsIioChannel**>(realloc(file->channels, sizeof(VfsIioChannel*) * new_num_channels));
	if (!new_channels_array) {
		free(new_channel);
		return -1;
	}

	file->channels = new_channels_array;
	file->channels[old_num_channels] = new_channel;
	file->num_channels = new_num_channels;

	if (cache_create(file, old_num_channels) != 0) {
		file->channels[old_num_channels] = nullptr;
		free(new_channel);
		file->num_channels = old_num_channels;
		return -1;
	}
	return old_num_channels;
}

VfsIioFile* vfs_iio_create(const char* fileName) {
	vfs_iio_CLOCK++;
	vfs_iio_BLOCK_SIZEv = get_page_size();
	if (vfs_iio_BLOCK_SIZEv < 256) {
		vfs_iio_BLOCK_SIZEv = 256;
	}

	VfsIioFile* newfile = static_cast<VfsIioFile*>(malloc(sizeof(VfsIioFile)));
	if (!newfile) {
		return nullptr;
	}

	newfile->file_name = static_cast<char*>(malloc(strlen(fileName) + 1));
	if (!newfile->file_name) {
		free(newfile);
		return nullptr;
	}
	strcpy(newfile->file_name, fileName);

	newfile->num_channels = 0;
	newfile->channels = nullptr;

	newfile->file_handle = fopen(fileName, "w+b");
	if (!newfile->file_handle) {
		free(newfile->file_name);
		free(newfile);
		return nullptr;
	}

	if (write_header(newfile) != 0) {
		fclose(newfile->file_handle);
		free(newfile->file_name);
		free(newfile);
		return nullptr;
	}

	return newfile;
}

VfsIioFile* vfs_iio_open(const char* fileName) {
	vfs_iio_CLOCK++;
	vfs_iio_BLOCK_SIZEv = get_page_size();
	if (vfs_iio_BLOCK_SIZEv < 256) {
		vfs_iio_BLOCK_SIZEv = 256;
	}

	VfsIioFile* openedfile = static_cast<VfsIioFile*>(malloc(sizeof(VfsIioFile)));
	if (!openedfile) {
		return nullptr;
	}

	openedfile->file_name = static_cast<char*>(malloc(strlen(fileName) + 1));
	if (!openedfile->file_name) {
		free(openedfile);
		return nullptr;
	}
	strcpy(openedfile->file_name, fileName);
	openedfile->num_channels = 0;
	openedfile->channels = nullptr;

	const char* mode;
	if ((vfs_iio_IOMODE & 2) != 0) {
		mode = "r+b";
	}
	else {
		mode = "rb";
	}

	openedfile->file_handle = fopen(fileName, mode);
	if (!openedfile->file_handle) {
		free(openedfile->file_name);
		free(openedfile);
		return nullptr;
	}

	if (read_header(openedfile) != 0) {
		fclose(openedfile->file_handle);
		free(openedfile->file_name);
		if (openedfile->channels) free(openedfile->channels);
		free(openedfile);
		return nullptr;
	}

	return openedfile;
}

void vfs_iio_close(VfsIioFile* file_to_close) {
	if (!file_to_close) {
		return;
	}
	vfs_iio_CLOCK++;

	flush_data(file_to_close);
	if (file_to_close->file_handle) {
		if (write_header(file_to_close) != 0) {
		}
		if ((vfs_iio_IOMODE & 2) != 0) {
			auto_truncate(file_to_close);
		}
		fclose(file_to_close->file_handle);
		file_to_close->file_handle = nullptr;
	}

	if (file_to_close->channels) {
		for (short i = 0; i < file_to_close->num_channels; ++i) {
			VfsIioChannel* channel = file_to_close->channels[i];
			if (channel) {
				if (channel->cache_header) {
					cache_destroy(channel->cache_header);
					channel->cache_header = nullptr;
				}
				free(channel);
			}
		}
		free(file_to_close->channels);
		file_to_close->channels = nullptr;
	}

	if (file_to_close->file_name) {
		free(file_to_close->file_name);
		file_to_close->file_name = nullptr;
	}
	free(file_to_close);
}

void vfs_iio_destroy(VfsIioFile* file_to_destroy) {
	if (!file_to_destroy) {
		return;
	}

	char* fileName_copy = nullptr;
	if (file_to_destroy->file_name) {
		size_t len = strlen(file_to_destroy->file_name);
		fileName_copy = static_cast<char*>(malloc(len + 1));
		if (fileName_copy) {
			strcpy(fileName_copy, file_to_destroy->file_name);
		}
	}

	vfs_iio_close(file_to_destroy);

	if (fileName_copy) {
		remove(fileName_copy);
		free(fileName_copy);
	}
}

int is_in_cache(VfsDataHandle* handle, long long block_index_to_check) {
	if (!handle || !handle->cache) {
		return 0;
	}
	VfsDataCacheHeader* cache = handle->cache;
	if (cache->buffer_capacity == 0) return 0;

	long long cache_start_block = cache->cache_window_start_offset / 512;
	long long cache_end_block =
		(cache->cache_window_start_offset + static_cast<long long>(cache->buffer_capacity)) / 512;

	if (block_index_to_check >= cache_start_block && block_index_to_check < cache_end_block) {
		return 1;
	}
	return 0;
}

int cache_flush(VfsDataHandle* handle) {
	if (!handle || !handle->cache || !handle->file_ptr) {
		return -1;
	}
	VfsDataCacheHeader* cache = handle->cache;

	if (cache->is_synced_flag != 1) {
		if (vfs_file_seek64(handle->file_ptr, cache->cache_window_start_offset) != 0) {
			return -1;
		}
		if (fwrite(cache->buffer, 1, cache->buffer_capacity, handle->file_ptr) != cache->buffer_capacity) {
		}
		cache->is_synced_flag = 1;
	}
	return 0;
}

int cache_slide(VfsDataHandle* handle, long long new_desired_start_offset) {
	++vfs_stat_data_slide;
	if (!handle || !handle->cache || !handle->file_ptr) {
		return -1;
	}
	VfsDataCacheHeader* cache = handle->cache;

	if (cache->is_synced_flag != 1) {
		if (cache_flush(handle) != 0) {
			return -1;
		}
	}

	long long aligned_new_start_offset = new_desired_start_offset;
	if (new_desired_start_offset % 512 != 0) {
		aligned_new_start_offset = (new_desired_start_offset / 512) * 512;
	}

	cache->cache_window_start_offset = aligned_new_start_offset;
	if (vfs_file_seek64(handle->file_ptr, cache->cache_window_start_offset) != 0) {
		return -1;
	}

	size_t bytes_read = fread(cache->buffer, 1, cache->buffer_capacity, handle->file_ptr);
	if (bytes_read < cache->buffer_capacity) {
		memset(static_cast<char*>(cache->buffer) + bytes_read, 0, cache->buffer_capacity - bytes_read);
	}

	cache->is_synced_flag = 1;

	return 0;
}

int cache_create(VfsDataHandle* handle) {
	if (!handle) {
		return -1;
	}

	handle->cache = static_cast<VfsDataCacheHeader*>(malloc(sizeof(VfsDataCacheHeader)));
	if (!handle->cache) {
		return -1;
	}

	VfsDataCacheHeader* cache = handle->cache;
	cache->buffer_capacity = (vfs_data_CACHE_BYTES > 0) ? static_cast<size_t>(vfs_data_CACHE_BYTES) : 0x10000;
	cache->buffer = malloc(cache->buffer_capacity);
	if (!cache->buffer) {
		free(cache);
		handle->cache = nullptr;
		return -1;
	}

	cache->is_synced_flag = 1;
	cache->cache_window_start_offset = 0;

	if (handle->file_ptr) {
		if (vfs_file_seek64(handle->file_ptr, cache->cache_window_start_offset) != 0) {
			free(cache->buffer);
			free(cache);
			handle->cache = nullptr;
			return -1;
		}
		const size_t bytes_read = fread(cache->buffer, 1, cache->buffer_capacity, handle->file_ptr);
		if (bytes_read < cache->buffer_capacity) {
			memset(static_cast<char*>(cache->buffer) + bytes_read, 0, cache->buffer_capacity - bytes_read);
		}
	}
	else {
		memset(cache->buffer, 0, cache->buffer_capacity);
	}
	return 0;
}

int cache_resize(VfsDataHandle* handle, size_t new_capacity) {
	if (!handle || !handle->cache || !handle->file_ptr) {
		return -1;
	}
	VfsDataCacheHeader* cache = handle->cache;

	if (cache->is_synced_flag != 1) {
		if (cache_flush(handle) != 0) return -1;
	}

	size_t aligned_new_capacity = new_capacity;
	if (new_capacity % 512 != 0) {
		aligned_new_capacity = (new_capacity / 512 + 1) * 512;
	}
	if (aligned_new_capacity == 0 && new_capacity > 0) {
		aligned_new_capacity = (new_capacity / 512 == (size_t)-1 / 512) ? new_capacity : (new_capacity / 512 + 1) * 512;
	}

	if (aligned_new_capacity <= 0) {
		aligned_new_capacity = 512;
	}

	void* new_buffer = realloc(cache->buffer, aligned_new_capacity);
	if (!new_buffer && aligned_new_capacity > 0) {
		return -1;
	}

	cache->buffer = new_buffer;
	cache->buffer_capacity = aligned_new_capacity;
	cache->is_synced_flag = 1;

	if (vfs_file_seek64(handle->file_ptr, cache->cache_window_start_offset) != 0) {
		return -1;
	}
	const size_t bytes_read = fread(cache->buffer, 1, cache->buffer_capacity, handle->file_ptr);
	if (bytes_read < cache->buffer_capacity) {
		memset(static_cast<char*>(cache->buffer) + bytes_read, 0, cache->buffer_capacity - bytes_read);
	}

	return 0;
}

int cache_destroy(VfsDataHandle* handle) {
	if (handle && handle->cache) {
		cache_flush(handle);
		VfsDataCacheHeader* cache = handle->cache;
		if (cache->buffer) {
			free(cache->buffer);
		}
		free(cache);
		handle->cache = nullptr;
	}
	return 0;
}

int cache_get(VfsDataHandle* handle, long long file_offset_to_read_from, int num_bytes_to_read, void* output_buffer) {
	if (!handle || !handle->cache || !output_buffer || num_bytes_to_read < 0) {
		return -1;
	}
	if (num_bytes_to_read == 0) {
		return 0;
	}

	VfsDataCacheHeader* cache = handle->cache;
	char* current_dest_ptr = static_cast<char*>(output_buffer);
	long long current_file_offset = file_offset_to_read_from;
	int bytes_remaining_to_read = num_bytes_to_read;

	while (bytes_remaining_to_read > 0) {
		if (!is_in_cache(handle, current_file_offset / 512)) {
			if (cache_slide(handle, current_file_offset) != 0) {
				return -1;
			}
		}

		long long offset_in_cache_buffer = current_file_offset - cache->cache_window_start_offset;
		long long bytes_available_in_window_from_offset =
			static_cast<long long>(cache->buffer_capacity) - offset_in_cache_buffer;

		if (offset_in_cache_buffer < 0 || bytes_available_in_window_from_offset <= 0) {
			return -1;
		}

		int bytes_to_copy_this_iteration = static_cast<int>(
			min(static_cast<long long>(bytes_remaining_to_read), bytes_available_in_window_from_offset));

		if (bytes_to_copy_this_iteration <= 0) break;

		memcpy(current_dest_ptr, static_cast<char*>(cache->buffer) + offset_in_cache_buffer,
			bytes_to_copy_this_iteration);

		current_dest_ptr += bytes_to_copy_this_iteration;
		current_file_offset += bytes_to_copy_this_iteration;
		bytes_remaining_to_read -= bytes_to_copy_this_iteration;
	}
	return 0;
}

int cache_put(VfsDataHandle* handle, long long file_offset_to_write_to, int num_bytes_to_write,
	const void* input_buffer) {
	if (!handle || !handle->cache || !input_buffer || num_bytes_to_write < 0) {
		return -1;
	}
	if (num_bytes_to_write == 0) {
		return 0;
	}

	VfsDataCacheHeader* cache = handle->cache;
	const char* current_src_ptr = static_cast<const char*>(input_buffer);
	long long current_file_offset = file_offset_to_write_to;
	int bytes_remaining_to_write = num_bytes_to_write;

	while (bytes_remaining_to_write > 0) {
		if (!is_in_cache(handle, current_file_offset / 512)) {
			if (cache_slide(handle, current_file_offset) != 0) {
				return -1;
			}
		}

		long long offset_in_cache_buffer = current_file_offset - cache->cache_window_start_offset;
		long long bytes_available_in_window_from_offset =
			static_cast<long long>(cache->buffer_capacity) - offset_in_cache_buffer;

		if (offset_in_cache_buffer < 0 || bytes_available_in_window_from_offset <= 0) {
			return -1;
		}

		int bytes_to_copy_this_iteration = static_cast<int>(
			min(static_cast<long long>(bytes_remaining_to_write), bytes_available_in_window_from_offset));

		if (bytes_to_copy_this_iteration <= 0) break;

		memcpy(static_cast<char*>(cache->buffer) + offset_in_cache_buffer, current_src_ptr,
			bytes_to_copy_this_iteration);
		cache->is_synced_flag = 0;

		current_src_ptr += bytes_to_copy_this_iteration;
		current_file_offset += bytes_to_copy_this_iteration;
		bytes_remaining_to_write -= bytes_to_copy_this_iteration;
	}
	return 0;
}

int vfs_data_read(VfsDataHandle* handle, int block_index, void* buffer) {
	++vfs_stat_data_read;
	if (!handle || !buffer || block_index < 0) {
		return -1;
	}

	const long long offset = static_cast<long long>(block_index) * 512LL;
	return cache_get(handle, offset, 512, buffer);
}

int vfs_data_write(VfsDataHandle* handle, int block_index, const void* buffer) {
	++vfs_stat_data_write;
	if (!handle || !buffer || block_index < 0) {
		return -1;
	}

	const long long offset = static_cast<long long>(block_index) * 512LL;
	return cache_put(handle, offset, 512, buffer);
}

int vfs_data_read_contiguous(VfsDataHandle* handle, int start_block_index, int num_blocks, void* buffer) {
	if (!handle || !buffer || start_block_index < 0 || num_blocks < 0) {
		return -1;
	}
	if (num_blocks == 0) return 0;

	const long long offset = static_cast<long long>(start_block_index) * 512LL;
	const long long bytes = static_cast<long long>(num_blocks) * 512LL;
	if (bytes > INT_MAX) {
		return -1;
	}

	return cache_get(handle, offset, static_cast<int>(bytes), buffer);
}

int vfs_data_write_contiguous(VfsDataHandle* handle, int start_block_index, int num_blocks, const void* buffer) {
	if (!handle || !buffer || start_block_index < 0 || num_blocks < 0) {
		return -1;
	}
	if (num_blocks == 0) return 0;

	const long long offset = static_cast<long long>(start_block_index) * 512LL;
	const long long bytes = static_cast<long long>(num_blocks) * 512LL;
	if (bytes > INT_MAX) {
		return -1;
	}

	return cache_put(handle, offset, static_cast<int>(bytes), buffer);
}

int vfs_data_flush_cache(VfsDataHandle* handle) {
	if (handle && handle->cache && handle->cache->is_synced_flag != 1) {
		cache_flush(handle);
	}
	return 0;
}

int vfs_data_set_cache_size(VfsDataHandle* handle, size_t new_size) {
	if (!handle) return -1;
	vfs_data_flush_cache(handle);
	return cache_resize(handle, new_size);
}

VfsDataHandle* vfs_data_create(const char* fileName) {
	if (fileName == nullptr) {
		return nullptr;
	}
	VfsDataHandle* handle = static_cast<VfsDataHandle*>(malloc(sizeof(VfsDataHandle)));
	if (!handle) {
		return nullptr;
	}

	handle->file_name = static_cast<char*>(malloc(strlen(fileName) + 1));
	if (!handle->file_name) {
		free(handle);
		return nullptr;
	}
	strcpy(handle->file_name, fileName);
	handle->cache = nullptr;

	handle->file_ptr = fopen(fileName, "w+b");
	if (!handle->file_ptr) {
		free(handle->file_name);
		free(handle);
		return nullptr;
	}

	if (cache_create(handle) != 0) {
		fclose(handle->file_ptr);
		free(handle->file_name);
		if (handle->cache) free(handle->cache);
		free(handle);
		return nullptr;
	}

	return handle;
}

VfsDataHandle* vfs_data_open(const char* fileName) {
	if (fileName == nullptr) {
		return nullptr;
	}
	VfsDataHandle* handle = static_cast<VfsDataHandle*>(malloc(sizeof(VfsDataHandle)));
	if (!handle) {
		return nullptr;
	}

	handle->file_name = static_cast<char*>(malloc(strlen(fileName) + 1));
	if (!handle->file_name) {
		free(handle);
		return nullptr;
	}
	strcpy(handle->file_name, fileName);
	handle->cache = nullptr;

	const char* open_mode;
	if ((vfs_data_IOMODE & 2) != 0) {
		open_mode = "r+b";
	}
	else {
		open_mode = "rb";
	}

	handle->file_ptr = fopen(fileName, open_mode);
	if (!handle->file_ptr) {
		free(handle->file_name);
		free(handle);
		return nullptr;
	}

	if (cache_create(handle) != 0) {
		fclose(handle->file_ptr);
		free(handle->file_name);
		if (handle->cache) free(handle->cache);
		free(handle);
		return nullptr;
	}
	return handle;
}

void vfs_data_close(VfsDataHandle* handle) {
	if (!handle) {
		return;
	}
	cache_destroy(handle);

	if (handle->file_ptr) {
		if (handle->file_ptr) fflush(handle->file_ptr);
		fclose(handle->file_ptr);
		handle->file_ptr = nullptr;
	}
	if (handle->file_name) {
		free(handle->file_name);
		handle->file_name = nullptr;
	}
	free(handle);
}

void vfs_data_destroy(VfsDataHandle* handle) {
	if (!handle) {
		return;
	}
	char* file_name_copy = nullptr;
	if (handle->file_name) {
		size_t len = strlen(handle->file_name);
		file_name_copy = static_cast<char*>(malloc(len + 1));
		if (file_name_copy) {
			strcpy(file_name_copy, handle->file_name);
		}
	}

	vfs_data_close(handle);

	if (file_name_copy) {
		remove(file_name_copy);
		free(file_name_copy);
	}
}

namespace {

bool fat_bits_reserve(VfsFatHandle* fat, int entries) {
	if (!fat || entries < 0) return false;
	if (entries > (1 << 30)) return false;

	const int need_words = (entries + 31) / 32;
	if (need_words <= fat->free_words && fat->free_bits) return true;

	int new_words = fat->free_words ? fat->free_words : 1024;
	while (new_words < need_words) new_words *= 2;

	unsigned int* p =
		static_cast<unsigned int*>(realloc(fat->free_bits, static_cast<size_t>(new_words) * sizeof(unsigned int)));
	if (!p) return false;

	memset(p + fat->free_words, 0xFF, static_cast<size_t>(new_words - fat->free_words) * sizeof(unsigned int));
	fat->free_bits = p;
	fat->free_words = new_words;
	fat->free_bits[0] &= ~1u;
	return true;
}

void fat_bits_mark(VfsFatHandle* fat, int idx, bool is_free) {
	if (!fat || !fat->free_bits || idx < 0) return;
	if (idx >= fat->free_words * 32) {
		if (!fat_bits_reserve(fat, idx + 1)) return;
	}
	if (idx == 0) return;
	const int w = idx >> 5;
	const unsigned int bit = 1u << (idx & 31);
	if (is_free)
		fat->free_bits[w] |= bit;
	else
		fat->free_bits[w] &= ~bit;
}

bool fat_bits_build(VfsFatHandle* fat) {
	if (!fat || !fat->iio_file) return false;

	VfsIioChannel* ch = vfs_iio_get_channel(fat->iio_file, fat->fat_iio_channel_id);
	const int channel_bytes = ch ? vfs_iio_channel_size(ch) : 0;
	const int entries = (channel_bytes > 0) ? channel_bytes / static_cast<int>(sizeof(int)) : 0;

	if (!fat_bits_reserve(fat, entries + 32768)) return false;

	const int kChunk = 16384;
	int* buf = static_cast<int*>(malloc(static_cast<size_t>(kChunk) * sizeof(int)));
	if (!buf) {
		return false;
	}

	for (int i = 0; i < entries; i += kChunk) {
		const int want = (entries - i < kChunk) ? (entries - i) : kChunk;
		vfs_iio_seek(fat->iio_file, fat->fat_iio_channel_id, i * static_cast<int>(sizeof(int)));
		const int got_bytes =
			vfs_iio_read(fat->iio_file, fat->fat_iio_channel_id, buf, want * static_cast<int>(sizeof(int)));
		const int got = (got_bytes > 0) ? got_bytes / static_cast<int>(sizeof(int)) : 0;
		for (int j = 0; j < got; ++j) {
			if (buf[j] != 0) fat_bits_mark(fat, i + j, false);
		}
		if (got < want) break;
	}
	free(buf);
	return true;
}

int fat_bits_find_free(VfsFatHandle* fat, int from) {
	if (!fat) return -1;
	if (from < 1) from = 1;
	if (!fat_bits_reserve(fat, from + 1)) return -1;

	int idx = from;
	const int total_bits = fat->free_words * 32;
	while (idx < total_bits) {
		const int w = idx >> 5;
		unsigned int word = fat->free_bits[w];
		const int bit = idx & 31;
		if (bit) word &= (0xFFFFFFFFu << bit);
		if (word) {
			int b = 0;
			while (!((word >> b) & 1u)) ++b;
			return (w << 5) + b;
		}
		idx = (w + 1) << 5;
	}

	const int next = total_bits;
	if (!fat_bits_reserve(fat, next + 32768)) return -1;
	return next;
}

}

int node_get_value(VfsFatHandle* fat_handle, int fat_entry_index) {
	++vfs_stat_fat_read;
	if (!fat_handle || !fat_handle->iio_file) return -2;

	int value_read = 0;
	vfs_iio_seek(fat_handle->iio_file, fat_handle->fat_iio_channel_id, fat_entry_index * sizeof(int));

	int bytes = vfs_iio_read(fat_handle->iio_file, fat_handle->fat_iio_channel_id, &value_read, sizeof(int));

	if (bytes != sizeof(int)) {
		return (bytes >= 0) ? 0 : -2;
	}
	return value_read;
}

void node_set_value(VfsFatHandle* fat_handle, int fat_entry_index, int value_to_set) {
	++vfs_stat_fat_write;
	if (!fat_handle || !fat_handle->iio_file) {
		return;
	}
	vfs_iio_seek(fat_handle->iio_file, fat_handle->fat_iio_channel_id, fat_entry_index * sizeof(int));
	vfs_iio_write(fat_handle->iio_file, fat_handle->fat_iio_channel_id, &value_to_set, sizeof(int));

	fat_bits_mark(fat_handle, fat_entry_index, value_to_set == 0);
}

int next_free(VfsFatHandle* fat_handle, int start_search_idx) {
	if (!fat_handle) return -1;

	if (!fat_handle->free_bits) {
		fat_bits_build(fat_handle);
	}

	if (fat_handle->free_bits) {
		const int found = fat_bits_find_free(fat_handle, start_search_idx);
		if (found > 0) return found;
	}

	int current_idx = start_search_idx;
	while (current_idx <= 0 || node_get_value(fat_handle, current_idx) != 0) {
		++vfs_stat_fat_scan_steps;
		current_idx++;
	}
	return current_idx;
}

int find_last_in_chain(VfsFatHandle* fat_handle, int start_of_chain_idx) {
	int previous_idx_in_chain;
	int current_idx_in_chain = start_of_chain_idx;

	if (current_idx_in_chain == -1) return start_of_chain_idx;

	do {
		previous_idx_in_chain = current_idx_in_chain;
		current_idx_in_chain = node_get_value(fat_handle, previous_idx_in_chain);
	} while (current_idx_in_chain != -1);

	return previous_idx_in_chain;
}

int node_recover(VfsFatHandle* fat_handle, int fat_entry_idx_to_free) {
	if (fat_entry_idx_to_free > 0) {
		node_set_value(fat_handle, fat_entry_idx_to_free, 0);
	}

	if (fat_handle && fat_entry_idx_to_free > 0 && fat_entry_idx_to_free < fat_handle->next_free_search_start_idx) {
		fat_handle->next_free_search_start_idx = fat_entry_idx_to_free;
	}
	return 1;
}

void vfs_fat_chain_for_each(VfsFatHandle* fat_handle, int start_of_chain_idx, int (*callback)(VfsFatHandle*, int)) {
	if (!fat_handle || !fat_handle->iio_file || !callback || start_of_chain_idx <= 0) {
		if (start_of_chain_idx == -1 && callback) {
			return;
		}
		if (start_of_chain_idx <= 0 && start_of_chain_idx != -1) return;
	}

	int current_entry_idx = start_of_chain_idx;
	int next_entry_idx;
	bool problem_encountered = false;

	while (current_entry_idx != -1) {
		next_entry_idx = node_get_value(fat_handle, current_entry_idx);

		if (callback(fat_handle, current_entry_idx) == 0) {
			break;
		}

		if (current_entry_idx <= 0) {
			problem_encountered = true;
		}

		current_entry_idx = next_entry_idx;
		if (problem_encountered) {
			break;
		}
		if (current_entry_idx != -1 && current_entry_idx <= 0) {
			problem_encountered = true;
		}
	}
}

int vfs_fat_chain_get_nth(VfsFatHandle* fat_handle, int start_of_chain_idx, int n) {
	if (!fat_handle || !fat_handle->iio_file || n < 0 || start_of_chain_idx <= 0) {
		return -1;
	}

	int current_entry_idx = start_of_chain_idx;
	for (int i = 0; i < n; ++i) {
		if (current_entry_idx == -1 || current_entry_idx <= 0) {
			return -1;
		}
		current_entry_idx = node_get_value(fat_handle, current_entry_idx);
	}
	return current_entry_idx;
}

void vfs_fat_chain_get(VfsFatHandle* fat_handle, int start_of_chain_idx, void* output_buffer) {
	if (!fat_handle || !fat_handle->iio_file || !output_buffer) {
		return;
	}

	int current_block_idx = start_of_chain_idx;
	int* out_ptr = static_cast<int*>(output_buffer);

	while (current_block_idx != -1 && current_block_idx > 0) {
		*out_ptr = current_block_idx;
		out_ptr++;
		current_block_idx = node_get_value(fat_handle, current_block_idx);
	}
}

void vfs_fat_chain_get_first_n(VfsFatHandle* fat_handle, int start_of_chain_idx, int num_entries_to_get,
	void* output_buffer) {
	if (!fat_handle || !fat_handle->iio_file || !output_buffer || num_entries_to_get <= 0) {
		return;
	}

	int current_block_idx = start_of_chain_idx;
	int* out_ptr = static_cast<int*>(output_buffer);
	int entries_retrieved = 0;

	while (entries_retrieved < num_entries_to_get) {
		if (current_block_idx == -1 || current_block_idx <= 0) {
			break;
		}
		*out_ptr = current_block_idx;
		out_ptr++;
		entries_retrieved++;
		if (entries_retrieved < num_entries_to_get) {
			current_block_idx = node_get_value(fat_handle, current_block_idx);
		}
	}
}

int vfs_fat_create_chain(VfsFatHandle* fat_handle) {
	if (!fat_handle || !fat_handle->iio_file) {
		return -1;
	}

	int new_chain_start_idx = fat_handle->next_free_search_start_idx;

	if (new_chain_start_idx <= 0) {
		new_chain_start_idx = next_free(fat_handle, 1);
		if (new_chain_start_idx <= 0) return -1;
	}

	if (node_get_value(fat_handle, new_chain_start_idx) != 0) {
		new_chain_start_idx = next_free(fat_handle, new_chain_start_idx + 1);
		if (new_chain_start_idx <= 0) return -1;
	}

	node_set_value(fat_handle, new_chain_start_idx, -1);

	fat_handle->next_free_search_start_idx = next_free(fat_handle, new_chain_start_idx);

	return new_chain_start_idx;
}

int vfs_fat_destroy_chain(VfsFatHandle* fat_handle, int start_of_chain_idx) {
	if (!fat_handle || !fat_handle->iio_file || start_of_chain_idx <= 0) {
		if (start_of_chain_idx == -1) return 0;
		return -1;
	}
	vfs_fat_chain_for_each(fat_handle, start_of_chain_idx, node_recover);
	return 0;
}

int vfs_fat_chain_extend(VfsFatHandle* fat_handle, int chain_to_extend) {
	if (!fat_handle || !fat_handle->iio_file) {
		return -1;
	}

	int new_block_idx = fat_handle->next_free_search_start_idx;
	if (new_block_idx <= 0 || node_get_value(fat_handle, new_block_idx) != 0) {
		new_block_idx = next_free(fat_handle, new_block_idx > 0 ? new_block_idx + 1 : 1);
		if (new_block_idx <= 0) return -1;
	}

	node_set_value(fat_handle, new_block_idx, -1);
	fat_handle->next_free_search_start_idx = next_free(fat_handle, new_block_idx);

	if (chain_to_extend <= 0) {
		return new_block_idx;
	}

	int last_block_in_chain = find_last_in_chain(fat_handle, chain_to_extend);
	if (last_block_in_chain == -1 || last_block_in_chain <= 0) {
		node_set_value(fat_handle, last_block_in_chain, new_block_idx);
	}
	else {
		node_set_value(fat_handle, last_block_in_chain, new_block_idx);
	}

	return new_block_idx;
}

int vfs_fat_chain_truncate(VfsFatHandle* fat_handle, int entry_idx_to_become_new_eoc) {
	if (!fat_handle || !fat_handle->iio_file || entry_idx_to_become_new_eoc <= 0) {
		return -1;
	}

	int next_block_after_trunc_point = node_get_value(fat_handle, entry_idx_to_become_new_eoc);
	node_set_value(fat_handle, entry_idx_to_become_new_eoc, -1);

	if (next_block_after_trunc_point != -1 && next_block_after_trunc_point > 0) {
		vfs_fat_destroy_chain(fat_handle, next_block_after_trunc_point);
	}
	return 0;
}

int vfs_fat_chain_shrink(VfsFatHandle* fat_handle, int start_of_chain_idx, int num_blocks_to_keep) {
	if (!fat_handle || !fat_handle->iio_file || num_blocks_to_keep < 0) {
		return -1;
	}
	if (start_of_chain_idx <= 0 && start_of_chain_idx != -1) return -1;
	if (start_of_chain_idx == -1) return 0;

	if (num_blocks_to_keep == 0) {
		return vfs_fat_destroy_chain(fat_handle, start_of_chain_idx);
	}

	int new_eoc_node_idx = vfs_fat_chain_get_nth(fat_handle, start_of_chain_idx, num_blocks_to_keep - 1);

	if (new_eoc_node_idx == -1 || new_eoc_node_idx <= 0) {
		int current_len = 0;
		int temp_iter = start_of_chain_idx;
		while (temp_iter != -1 && temp_iter > 0) {
			current_len++;
			if (current_len > num_blocks_to_keep + 5 && num_blocks_to_keep > 0) break;
			temp_iter = node_get_value(fat_handle, temp_iter);
		}
		if (current_len < num_blocks_to_keep) return -1;
		if (new_eoc_node_idx <= 0 && new_eoc_node_idx != -1) return -1;
		return 0;
	}

	return vfs_fat_chain_truncate(fat_handle, new_eoc_node_idx);
}

VfsFatHandle* vfs_fat_create(VfsIioFile* iio_file, int num_iio_blocks_for_fat_channel) {
	if (!iio_file) {
		return nullptr;
	}

	int blocks_for_fat_stripe = (num_iio_blocks_for_fat_channel == 0) ? 4 : num_iio_blocks_for_fat_channel;
	int fat_channel_id = vfs_iio_allocate_channel(iio_file, blocks_for_fat_stripe);

	if (fat_channel_id < 0) {
		return nullptr;
	}

	VfsFatHandle* fat_handle = static_cast<VfsFatHandle*>(malloc(sizeof(VfsFatHandle)));
	if (!fat_handle) {
		return nullptr;
	}

	fat_handle->iio_file = iio_file;
	fat_handle->fat_iio_channel_id = fat_channel_id;
	fat_handle->next_free_search_start_idx = 1;
	fat_handle->free_bits = nullptr;
	fat_handle->free_words = 0;

	return fat_handle;
}

VfsFatHandle* vfs_fat_open(VfsIioFile* iio_file, int fat_iio_channel_id) {
	if (!iio_file) {
		return nullptr;
	}

	VfsFatHandle* fat_handle = static_cast<VfsFatHandle*>(malloc(sizeof(VfsFatHandle)));
	if (!fat_handle) {
		return nullptr;
	}

	fat_handle->iio_file = iio_file;
	fat_handle->fat_iio_channel_id = fat_iio_channel_id;
	fat_handle->free_bits = nullptr;
	fat_handle->free_words = 0;

	fat_handle->next_free_search_start_idx = 1;

	return fat_handle;
}

int vfs_fat_close(VfsFatHandle* fat_handle) {
	if (fat_handle) {
		free(fat_handle->free_bits);
		free(fat_handle);
	}
	return 0;
}

int node_get(VfsNtHandle* nt_handle, int node_index, VfsNode* node_buffer) {
	if (!nt_handle || !nt_handle->iio_file || !node_buffer) {
		return -1;
	}
	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, node_index * sizeof(VfsNode));
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, node_buffer, sizeof(VfsNode));
	if (bytes_read < (int)sizeof(VfsNode)) {
		int offset = bytes_read > 0 ? bytes_read : 0;
		memset((char*)node_buffer + offset, 0, sizeof(VfsNode) - offset);
		return sizeof(VfsNode);
	}
	return bytes_read;
}

int node_set(VfsNtHandle* nt_handle, int node_index, const VfsNode* node_data) {
	if (!nt_handle || !nt_handle->iio_file || !node_data) {
		return -1;
	}
	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, node_index * sizeof(VfsNode));
	return vfs_iio_write(nt_handle->iio_file, nt_handle->nt_iio_channel_id, node_data, sizeof(VfsNode));
}

int find_first_free(VfsNtHandle* nt_handle, int start_search_idx) {
	if (!nt_handle) return -1;

	int current_idx = start_search_idx;
	VfsNode temp_node;

	if (node_get(nt_handle, current_idx, &temp_node) != sizeof(VfsNode)) {
	}

	while (temp_node.ref_count > 0) {
		current_idx++;
		if (node_get(nt_handle, current_idx, &temp_node) != sizeof(VfsNode)) {
			return -1;
		}
		if (current_idx > start_search_idx + 10000000) {
			return -1;
		}
	}
	return current_idx;
}

int node_recover(VfsNtHandle* nt_handle, int node_idx_to_free) {
	if (!nt_handle) return -1;

	VfsNode zero_node;
	memset(&zero_node, 0, sizeof(VfsNode));

	node_set(nt_handle, node_idx_to_free, &zero_node);

	if (node_idx_to_free < nt_handle->next_free_node_search_start_idx) {
		nt_handle->next_free_node_search_start_idx = node_idx_to_free;
	}
	return 0;
}

int vfs_nt_get_node(VfsNtHandle* nt_handle, int node_index, VfsNode* node_buffer) {
	if (!nt_handle) {
		return -1;
	}
	return node_get(nt_handle, node_index, node_buffer);
}

int vfs_nt_set_node(VfsNtHandle* nt_handle, int node_index, const VfsNode* node_data) {
	if (!nt_handle) {
		return -1;
	}
	return node_set(nt_handle, node_index, node_data);
}

int vfs_nt_allocate_node(VfsNtHandle* nt_handle) {
	if (!nt_handle) {
		return -1;
	}

	int start_search = nt_handle->next_free_node_search_start_idx;
	if (start_search < 0) start_search = 0;
	int new_node_idx = find_first_free(nt_handle, start_search);
	if (new_node_idx < 0) return -1;

	VfsNode new_node;
	new_node.ref_count = 1;
	new_node.file_size_bytes = 0;
	new_node.fat_chain_start_idx = 0;
	new_node.user_flags_or_type = 0;

	if (vfs_nt_set_node(nt_handle, new_node_idx, &new_node) < 0) {
		return -1;
	}

	nt_handle->next_free_node_search_start_idx = find_first_free(nt_handle, new_node_idx);

	return new_node_idx;
}

int vfs_nt_node_get_size(VfsNtHandle* nt_handle, int node_index) {
	if (!nt_handle) {
		return -1;
	}

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));

	VfsNode current_node;
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &current_node, sizeof(VfsNode));

	if (bytes_read < (int)sizeof(VfsNode)) {
		return -1;
	}

	return current_node.file_size_bytes;
}

void vfs_nt_node_set_size(VfsNtHandle* nt_handle, int node_index, int new_size) {
	if (!nt_handle || !nt_handle->iio_file) {
		return;
	}

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));

	VfsNode node_buffer;
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &node_buffer, sizeof(VfsNode));

	if (bytes_read != (int)sizeof(VfsNode)) {
		return;
	}

	node_buffer.file_size_bytes = new_size;

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));
	vfs_iio_write(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &node_buffer, sizeof(VfsNode));
}

int vfs_nt_node_get_chain(VfsNtHandle* nt_handle, int node_index) {
	if (!nt_handle) {
		return -1;
	}

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, node_index * (int)sizeof(VfsNode));

	VfsNode node_buffer;
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &node_buffer, sizeof(VfsNode));

	if (bytes_read != (int)sizeof(VfsNode)) {
		return -1;
	}

	return node_buffer.fat_chain_start_idx;
}

void vfs_nt_node_set_chain(VfsNtHandle* nt_handle, int node_index, int new_fat_chain_start_idx) {
	if (!nt_handle || !nt_handle->iio_file) {
		return;
	}

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));

	VfsNode node_buffer;
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &node_buffer, sizeof(VfsNode));

	if (bytes_read != (int)sizeof(VfsNode)) {
		return;
	}

	node_buffer.fat_chain_start_idx = new_fat_chain_start_idx;

	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));
	vfs_iio_write(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &node_buffer, sizeof(VfsNode));
}

void vfs_nt_refcount_incr(VfsNtHandle* nt_handle, int node_index) {
	if (!nt_handle) {
		return;
	}
	VfsNode current_node;
	if (vfs_nt_get_node(nt_handle, node_index, &current_node) >= 0) {
		if (current_node.ref_count > 0) {
			current_node.ref_count++;
			vfs_nt_set_node(nt_handle, node_index, &current_node);
		}
	}
}

int vfs_nt_refcount_decr(VfsNtHandle* nt_handle, int node_index) {
	if (!nt_handle) {
		return 0;
	}

	VfsNode current_node;
	vfs_iio_seek(nt_handle->iio_file, nt_handle->nt_iio_channel_id, (long long)node_index * sizeof(VfsNode));
	int bytes_read = vfs_iio_read(nt_handle->iio_file, nt_handle->nt_iio_channel_id, &current_node, sizeof(VfsNode));
	if (bytes_read < (int)sizeof(VfsNode)) {
		return 0;
	}

	if (current_node.ref_count > 0) {
		current_node.ref_count--;
		vfs_nt_set_node(nt_handle, node_index, &current_node);
		if (current_node.ref_count == 0) {
			node_recover(nt_handle, node_index);
			return 1;
		}
		return 0;
	}

	vfs_nt_set_node(nt_handle, node_index, &current_node);
	node_recover(nt_handle, node_index);
	return 1;
}

VfsNtHandle* vfs_nt_create(VfsIioFile* iio_file, int num_iio_blocks_for_nt_channel) {
	if (!iio_file) {
		return nullptr;
	}

	int blocks_for_nt_stripe = (num_iio_blocks_for_nt_channel == 0) ? 1 : num_iio_blocks_for_nt_channel;
	int nt_channel_id = vfs_iio_allocate_channel(iio_file, blocks_for_nt_stripe);

	if (nt_channel_id < 0) {
		return nullptr;
	}

	VfsNtHandle* nt_handle = static_cast<VfsNtHandle*>(malloc(sizeof(VfsNtHandle)));
	if (!nt_handle) {
		return nullptr;
	}

	nt_handle->iio_file = iio_file;
	nt_handle->nt_iio_channel_id = nt_channel_id;
	nt_handle->next_free_node_search_start_idx = 0;

	return nt_handle;
}

VfsNtHandle* vfs_nt_open(VfsIioFile* iio_file, int nt_iio_channel_id) {
	if (!iio_file) {
		return nullptr;
	}

	VfsNtHandle* nt_handle = static_cast<VfsNtHandle*>(malloc(sizeof(VfsNtHandle)));
	if (!nt_handle) {
		return nullptr;
	}

	nt_handle->iio_file = iio_file;
	nt_handle->nt_iio_channel_id = nt_iio_channel_id;
	nt_handle->next_free_node_search_start_idx = find_first_free(nt_handle, 0);

	return nt_handle;
}

void vfs_nt_close(VfsNtHandle* nt_handle) {
	if (nt_handle) {
		free(nt_handle);
	}
}

void vfs_nt_destroy(VfsNtHandle* nt_handle) {
	vfs_nt_close(nt_handle);
}

int trienode_get(VfsDtHandle* dt_handle, int tn_idx, VfsTrieNode* out_node) {
	if (!dt_handle || !dt_handle->iio_file || !out_node || tn_idx < 0) return -1;

	const int rec = static_cast<int>(sizeof(VfsTrieNode));
	vfs_iio_seek(dt_handle->iio_file, dt_handle->trienode_channel_id, tn_idx * rec);

	VfsTrieNode disk;
	if (vfs_iio_read(dt_handle->iio_file, dt_handle->trienode_channel_id, &disk, rec) != rec) return -1;
	*out_node = disk;
	return 0;
}

int trienode_set(VfsDtHandle* dt_handle, int tn_idx, VfsTrieNode* node_to_set) {
	if (!dt_handle || !dt_handle->iio_file || !node_to_set || tn_idx < 0) return -1;

	const int rec = static_cast<int>(sizeof(VfsTrieNode));
	vfs_iio_seek(dt_handle->iio_file, dt_handle->trienode_channel_id, tn_idx * rec);
	return (vfs_iio_write(dt_handle->iio_file, dt_handle->trienode_channel_id, node_to_set, rec) == rec) ? 0 : -1;
}

int keynode_get(VfsDtHandle* dt_handle, int kn_idx, VfsKeyNode* out_node) {
	if (!dt_handle || !dt_handle->iio_file || !out_node) return -1;
	vfs_iio_seek(dt_handle->iio_file, dt_handle->keynode_channel_id, kn_idx * sizeof(VfsKeyNode));
	return vfs_iio_read(dt_handle->iio_file, dt_handle->keynode_channel_id, out_node, sizeof(VfsKeyNode));
}

int keynode_set(VfsDtHandle* dt_handle, int kn_idx, const VfsKeyNode* node_to_set) {
	if (!dt_handle || !dt_handle->iio_file || !node_to_set) return -1;
	vfs_iio_seek(dt_handle->iio_file, dt_handle->keynode_channel_id, kn_idx * sizeof(VfsKeyNode));
	return vfs_iio_write(dt_handle->iio_file, dt_handle->keynode_channel_id, node_to_set, sizeof(VfsKeyNode));
}

int trienode_is_free(VfsDtHandle* dt_handle, int tn_idx) {
	if (tn_idx == 0) {
		return 0;
	}
	VfsTrieNode temp_node;
	if (trienode_get(dt_handle, tn_idx, &temp_node) == 0) {
		return (temp_node.k_index >= 0);
	}
	return 0;
}

int trienode_find_first_free(VfsDtHandle* dt_handle, int start_idx) {
	if (!dt_handle || !dt_handle->iio_file) return -1;
	VfsIioChannel* ch = vfs_iio_get_channel(dt_handle->iio_file, dt_handle->trienode_channel_id);
	if (!ch) return -1;
	int channel_bytes = vfs_iio_channel_size(ch);
	int entry_size = static_cast<int>(sizeof(VfsTrieNode));
	int num_entries = (channel_bytes > 0) ? (channel_bytes / entry_size) : 0;

	int idx = (start_idx > 0) ? start_idx : 1;
	int max_idx = idx + 1000000;

	for (; idx > 0 && idx <= max_idx; ++idx) {
		if (idx >= num_entries) {
			return idx;
		}
		if (trienode_is_free(dt_handle, idx)) {
			return idx;
		}
	}
	return -1;
}

int trienode_clear(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode cleared_node;
	cleared_node.nt_idx = 0;
	cleared_node.b_index = 0;
	cleared_node.k_index = 0;
	cleared_node.left_child_idx = tn_idx;
	cleared_node.right_child_idx = tn_idx;
	return trienode_set(dt_handle, tn_idx, &cleared_node);
}

int trienode_recover(VfsDtHandle* dt_handle, int tn_idx) {
	if (!dt_handle || tn_idx < 0) return -1;
	if (tn_idx == 0) return -1;

	trienode_clear(dt_handle, tn_idx);
	if (tn_idx > 0 && tn_idx < dt_handle->next_free_trienode_idx) {
		dt_handle->next_free_trienode_idx = tn_idx;
	}
	return 0;
}

int keynode_is_free(VfsDtHandle* dt_handle, int kn_idx) {
	if (kn_idx == 0) {
		return 0;
	}
	VfsKeyNode temp_node;
	if (keynode_get(dt_handle, kn_idx, &temp_node) == sizeof(VfsKeyNode)) {
		return (temp_node.next_fragment_idx_flags >= 0);
	}
	return 0;
}

int keynode_find_first_free(VfsDtHandle* dt_handle, int start_idx) {
	if (!dt_handle || !dt_handle->iio_file) return -1;
	int idx = (start_idx > 0) ? start_idx : 1;

	VfsIioChannel* ch = vfs_iio_get_channel(dt_handle->iio_file, dt_handle->keynode_channel_id);
	int channel_bytes = vfs_iio_channel_size(ch);
	int entry_size = sizeof(VfsKeyNode);
	int num_entries = (channel_bytes > 0) ? (channel_bytes / entry_size) : 0;

	int max_idx = (start_idx > 0) ? (start_idx + 1000000) : 1000000;

	for (; idx > 0 && idx <= max_idx; ++idx) {
		if (idx >= num_entries) {
			return idx;
		}
		if (keynode_is_free(dt_handle, idx)) {
			return idx;
		}
	}
	return -1;
}

int keynode_clear(VfsDtHandle* dt_handle, int kn_idx) {
	VfsKeyNode cleared_node;
	memset(&cleared_node, 0, sizeof(VfsKeyNode));
	return keynode_set(dt_handle, kn_idx, &cleared_node);
}

unsigned int trienode_get_left(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return 0;
	return static_cast<unsigned int>(node.left_child_idx);
}

unsigned int trienode_get_right(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return 0;
	return static_cast<unsigned int>(node.right_child_idx);
}

int trienode_get_bindex(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return 0;
	return node.b_index;
}

unsigned int trienode_get_kindex(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return 0;
	return static_cast<unsigned int>(node.k_index) & 0x7FFFFFFF;
}

int trienode_get_nt(VfsDtHandle* dt_handle, int tn_idx) {
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return 0;
	return node.nt_idx;
}

static bool trienode_index_in_range(VfsDtHandle* dt_handle, int tn_idx) {
	if (!dt_handle || !dt_handle->iio_file || tn_idx < 0) return false;
	VfsIioChannel* chan = vfs_iio_get_channel(dt_handle->iio_file, dt_handle->trienode_channel_id);
	if (!chan) return false;
	return tn_idx < chan->current_size_bytes / static_cast<int>(sizeof(VfsTrieNode));
}

int trienode_set_left(VfsDtHandle* dt_handle, int tn_idx, int left_child_idx) {
	if (!trienode_index_in_range(dt_handle, tn_idx)) return -1;
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return -1;
	node.left_child_idx = left_child_idx;
	return trienode_set(dt_handle, tn_idx, &node);
}

int trienode_set_right(VfsDtHandle* dt_handle, int tn_idx, int right_child_idx) {
	if (!trienode_index_in_range(dt_handle, tn_idx)) return -1;
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return -1;
	node.right_child_idx = right_child_idx;
	return trienode_set(dt_handle, tn_idx, &node);
}

int trienode_set_nt(VfsDtHandle* dt_handle, int tn_idx, int nt_idx) {
	if (!trienode_index_in_range(dt_handle, tn_idx)) return -1;
	VfsTrieNode node;
	if (trienode_get(dt_handle, tn_idx, &node) != 0) return -1;
	node.nt_idx = nt_idx;
	return trienode_set(dt_handle, tn_idx, &node);
}

int fnode_extract_key(VfsDtHandle* dt_handle, int start_keynode_idx, char* output_buffer) {
	if (!dt_handle || !output_buffer) {
		if (output_buffer) *output_buffer = '\0';
		return -1;
	}
	if (start_keynode_idx <= 0) {
		*output_buffer = '\0';
		return 0;
	}

	const int max_key_len = 4095;
	int output_pos = 0;
	int current_idx = start_keynode_idx;

	do {
		VfsKeyNode node;
		if (keynode_get(dt_handle, current_idx, &node) != sizeof(VfsKeyNode)) {
			output_buffer[output_pos] = '\0';
			return -1;
		}

		unsigned int next_idx = node.next_fragment_idx_flags & 0x7FFFFFFF;

		for (int i = 0; i < 60 && output_pos < max_key_len; ++i) {
			char c = node.key_fragment[i];
			if (c == '\0') {
				break;
			}
			output_buffer[output_pos++] = c;
		}

		current_idx = next_idx;
	} while (current_idx != 0 && output_pos < max_key_len);

	output_buffer[output_pos] = '\0';
	return 0;
}

int fnode_free(VfsDtHandle* dt_handle, int start_keynode_idx_to_free) {
	if (!dt_handle || start_keynode_idx_to_free <= 0) {
		return 0;
	}

	VfsKeyNode current_keynode;
	int current_kn_idx = start_keynode_idx_to_free;
	int next_kn_idx;

	while (current_kn_idx != 0) {
		if (keynode_get(dt_handle, current_kn_idx, &current_keynode) != sizeof(VfsKeyNode)) {
			return -1;
		}
		next_kn_idx = current_keynode.next_fragment_idx_flags & 0x7FFFFFFF;

		keynode_clear(dt_handle, current_kn_idx);

		if (current_kn_idx > 0 && current_kn_idx < dt_handle->next_free_keynode_idx) {
			dt_handle->next_free_keynode_idx = current_kn_idx;
		}
		current_kn_idx = next_kn_idx;
	}
	return 0;
}

int fnode_allocate(VfsDtHandle* dt_handle, const char* source_string) {
	if (!dt_handle || !source_string) return -1;

	size_t len_with_null = strlen(source_string) + 1;
	int num_fragments = static_cast<int>((len_with_null + 59) / 60);
	if (num_fragments == 0 && len_with_null == 1) {
		num_fragments = 1;
	}
	if (num_fragments == 0) return 0;

	const int MAX_FRAGMENTS_TEMP = 1024;
	if (num_fragments > MAX_FRAGMENTS_TEMP) return -1;
	int allocated_indices[MAX_FRAGMENTS_TEMP];

	for (int i = 0; i < num_fragments; ++i) {
		int new_kn_idx = dt_handle->next_free_keynode_idx;
		if (new_kn_idx <= 0 || !keynode_is_free(dt_handle, new_kn_idx)) {
			new_kn_idx = keynode_find_first_free(dt_handle, (new_kn_idx > 0) ? new_kn_idx : 1);
		}
		if (new_kn_idx <= 0) {
			for (int k = 0; k < i; ++k) keynode_clear(dt_handle, allocated_indices[k]);
			return -1;
		}
		allocated_indices[i] = new_kn_idx;
		dt_handle->next_free_keynode_idx = keynode_find_first_free(dt_handle, new_kn_idx + 1);
	}

	VfsKeyNode current_keynode;
	const char* current_source_pos = source_string;

	for (int i = 0; i < num_fragments; ++i) {
		if (i == num_fragments - 1) {
			current_keynode.next_fragment_idx_flags = 0x80000000;
		}
		else {
			current_keynode.next_fragment_idx_flags = allocated_indices[i + 1] | 0x80000000;
		}

		memset(current_keynode.key_fragment, 0, sizeof(current_keynode.key_fragment));
		int chars_to_copy_this_fragment = 0;
		for (int j = 0; j < 60; ++j) {
			if (*current_source_pos == '\0' && i == num_fragments - 1) {
				current_keynode.key_fragment[j] = '\0';
				chars_to_copy_this_fragment++;
				current_source_pos++;
				break;
			}
			if (*current_source_pos == '\0' && i < num_fragments - 1) {
				break;
			}
			if (*current_source_pos == '\0') break;

			current_keynode.key_fragment[j] = *current_source_pos;
			current_source_pos++;
			chars_to_copy_this_fragment++;
		}

		if (keynode_set(dt_handle, allocated_indices[i], &current_keynode) < 0) {
			for (int k = 0; k < num_fragments; ++k) keynode_clear(dt_handle, allocated_indices[k]);
			return -1;
		}
	}
	return (num_fragments > 0) ? allocated_indices[0] : 0;
}

int node_allocate(VfsDtHandle* dt_handle, const char* key_string, int nt_idx_for_trienode, int b_index_for_trienode) {
	if (!dt_handle || !key_string) return -1;

	int keynode_chain_start_idx = fnode_allocate(dt_handle, key_string);
	if (keynode_chain_start_idx < 0) return -1;
	if (keynode_chain_start_idx == 0 && strlen(key_string) > 0) {
		return -1;
	}

	int new_trienode_idx = dt_handle->next_free_trienode_idx;
	if (new_trienode_idx <= 0 || !trienode_is_free(dt_handle, new_trienode_idx)) {
		new_trienode_idx = trienode_find_first_free(dt_handle, new_trienode_idx);
	}
	if (new_trienode_idx <= 0) {
		if (keynode_chain_start_idx > 0) fnode_free(dt_handle, keynode_chain_start_idx);
		return -1;
	}

	VfsTrieNode new_trienode;
	new_trienode.nt_idx = nt_idx_for_trienode;
	new_trienode.b_index = b_index_for_trienode;
	new_trienode.k_index = keynode_chain_start_idx | 0x80000000;
	new_trienode.left_child_idx = 0;
	new_trienode.right_child_idx = 0;

	if (trienode_set(dt_handle, new_trienode_idx, &new_trienode) < 0) {
		if (keynode_chain_start_idx > 0) fnode_free(dt_handle, keynode_chain_start_idx);
		return -1;
	}

	dt_handle->next_free_trienode_idx = trienode_find_first_free(dt_handle, new_trienode_idx + 1);

	return new_trienode_idx;
}

int node_copy_key(VfsDtHandle* dt_handle, int source_trienode_idx, int dest_trienode_idx) {
	if (!dt_handle || source_trienode_idx <= 0 || dest_trienode_idx <= 0) {
		return -1;
	}

	VfsTrieNode source_node;
	VfsTrieNode dest_node_original_content;

	if (trienode_get(dt_handle, source_trienode_idx, &source_node) != 0 ||
		trienode_get(dt_handle, dest_trienode_idx, &dest_node_original_content) != 0) {
		return -1;
	}

	int old_dest_keynode_chain_start = dest_node_original_content.k_index & 0x7FFFFFFF;

	VfsTrieNode new_dest_content;
	new_dest_content.nt_idx = source_node.nt_idx;
	new_dest_content.b_index = source_node.b_index;
	new_dest_content.k_index = source_node.k_index;
	new_dest_content.left_child_idx = dest_node_original_content.left_child_idx;
	new_dest_content.right_child_idx = dest_node_original_content.right_child_idx;

	if (trienode_set(dt_handle, dest_trienode_idx, &new_dest_content) != 0) {
		return -1;
	}

	if (old_dest_keynode_chain_start > 0 && (dest_node_original_content.k_index < 0)) {
		fnode_free(dt_handle, old_dest_keynode_chain_start);
	}
	return 0;
}

int p_get_head() {
	return 0;
}

int p_init_head(VfsDtHandle* dt_handle) {
	if (!dt_handle) return -1;

	VfsKeyNode head_keynode;
	memset(&head_keynode, 0, sizeof(VfsKeyNode));
	head_keynode.next_fragment_idx_flags = 0x80000000;

	VfsTrieNode head_trienode;
	head_trienode.nt_idx = 0;
	head_trienode.b_index = -1;
	head_trienode.k_index = 0 | 0x80000000;
	head_trienode.left_child_idx = 0;
	head_trienode.right_child_idx = 0;

	if (keynode_set(dt_handle, 0, &head_keynode) < 0) return -1;
	if (trienode_set(dt_handle, 0, &head_trienode) < 0) return -1;

	return 0;
}

int p_compare_keys(VfsDtHandle* dt_handle, const char* key_to_compare, int external_trienode_idx) {
	if (!dt_handle || !key_to_compare || external_trienode_idx < 0) return 0;

	unsigned int stored_key_kn_start_idx = trienode_get_kindex(dt_handle, external_trienode_idx);

	VfsTrieNode ext_node;
	if (trienode_get(dt_handle, external_trienode_idx, &ext_node) < 0) return 0;
	if (ext_node.k_index >= 0) {
		return (*key_to_compare == '\0');
	}
	if (stored_key_kn_start_idx == 0 && (ext_node.k_index != static_cast<int>(0x80000000))) {
		return (*key_to_compare == '\0');
	}

	char stored_key_buffer[4096];
	if (fnode_extract_key(dt_handle, stored_key_kn_start_idx, stored_key_buffer) < 0) {
		return 0;
	}

	return (strcmp(key_to_compare, stored_key_buffer) == 0) ? 1 : 0;
}

int p_find_first_different_bit(VfsDtHandle* dt_handle, const char* key1, int trienode_idx_for_key2) {
	if (!dt_handle || !key1 || trienode_idx_for_key2 < 0) return -1;

	unsigned int stored_key_kn_start_idx = trienode_get_kindex(dt_handle, trienode_idx_for_key2);
	VfsTrieNode ext_node;
	if (trienode_get(dt_handle, trienode_idx_for_key2, &ext_node) < 0) return -1;
	if (ext_node.k_index >= 0) return bitfirst_different(key1, "");

	char stored_key_buffer[4096];
	if (fnode_extract_key(dt_handle, stored_key_kn_start_idx, stored_key_buffer) < 0) {
		return -1;
	}

	return bitfirst_different(key1, stored_key_buffer);
}

int p_lookup_key(VfsDtHandle* dt_handle, const char* key_to_lookup) {
	if (!dt_handle || !key_to_lookup) return -1;

	int p = p_get_head();
	VfsTrieNode pn, xn;
	if (trienode_get(dt_handle, p, &pn) != 0) return -1;
	int x = pn.right_child_idx;
	if (trienode_get(dt_handle, x, &xn) != 0) return -1;

	while (pn.b_index < xn.b_index) {
		p = x;
		pn = xn;
		x = bit_get(key_to_lookup, pn.b_index) ? pn.right_child_idx : pn.left_child_idx;
		if (trienode_get(dt_handle, x, &xn) != 0) return -1;
	}

	if (p_compare_keys(dt_handle, key_to_lookup, x) == 1) {
		return x;
	}

	return -1;
}

int p_lookup_key_n(VfsDtHandle* dt_handle, const char* key_to_lookup, int num_bits_to_match) {
	if (!dt_handle || !key_to_lookup || num_bits_to_match < 0) return -1;

	int p = p_get_head();
	int x = trienode_get_right(dt_handle, p);

	while (trienode_get_bindex(dt_handle, p) < trienode_get_bindex(dt_handle, x) &&
		trienode_get_bindex(dt_handle, x) < num_bits_to_match) {
		p = x;
		x = bit_get(key_to_lookup, trienode_get_bindex(dt_handle, x)) ? trienode_get_right(dt_handle, x)
																	  : trienode_get_left(dt_handle, x);
	}
	return p;
}

int p_insert_key(VfsDtHandle* dt_handle, const char* key_to_insert, int nt_idx) {
	if (!dt_handle || !key_to_insert) return -1;

	int p = p_get_head();
	VfsTrieNode pn, xn;
	if (trienode_get(dt_handle, p, &pn) != 0) return -1;
	int x = pn.right_child_idx;
	if (trienode_get(dt_handle, x, &xn) != 0) return -1;

	while (pn.b_index < xn.b_index) {
		p = x;
		pn = xn;
		x = bit_get(key_to_insert, pn.b_index) ? pn.right_child_idx : pn.left_child_idx;
		if (trienode_get(dt_handle, x, &xn) != 0) return -1;
	}

	if (p_compare_keys(dt_handle, key_to_insert, x) == 1) {
		return -1;
	}

	int first_diff_bit = p_find_first_different_bit(dt_handle, key_to_insert, x);

	int p_insert = p_get_head();
	VfsTrieNode pi, xi;
	if (trienode_get(dt_handle, p_insert, &pi) != 0) return -1;
	int x_insert = pi.right_child_idx;
	if (trienode_get(dt_handle, x_insert, &xi) != 0) return -1;

	while (pi.b_index < xi.b_index && xi.b_index < first_diff_bit) {
		p_insert = x_insert;
		pi = xi;
		x_insert = bit_get(key_to_insert, pi.b_index) ? pi.right_child_idx : pi.left_child_idx;
		if (trienode_get(dt_handle, x_insert, &xi) != 0) return -1;
	}

	int new_node_idx = node_allocate(dt_handle, key_to_insert, nt_idx, first_diff_bit);
	if (new_node_idx < 0) {
		return -1;
	}

	int new_node_bit = bit_get(key_to_insert, first_diff_bit);
	if (new_node_bit) {
		trienode_set_left(dt_handle, new_node_idx, x_insert);
		trienode_set_right(dt_handle, new_node_idx, new_node_idx);
	}
	else {
		trienode_set_left(dt_handle, new_node_idx, new_node_idx);
		trienode_set_right(dt_handle, new_node_idx, x_insert);
	}

	if (trienode_get_right(dt_handle, p_insert) == x_insert) {
		trienode_set_right(dt_handle, p_insert, new_node_idx);
	}
	else {
		trienode_set_left(dt_handle, p_insert, new_node_idx);
	}

	return new_node_idx;
}

static int p_relink_upward(VfsDtHandle* dt_handle, const char* key, int from_node, int to_node) {
	if (!dt_handle || !key) return -1;

	int p = p_get_head();
	VfsTrieNode pn;
	if (trienode_get(dt_handle, p, &pn) != 0) return -1;

	if (pn.right_child_idx == from_node) {
		pn.right_child_idx = to_node;
		return trienode_set(dt_handle, p, &pn);
	}

	int x = pn.right_child_idx;
	VfsTrieNode xn;
	if (trienode_get(dt_handle, x, &xn) != 0) return -1;

	while (pn.b_index < xn.b_index) {
		p = x;
		pn = xn;
		const int go_right = bit_get(key, pn.b_index);
		const int next = go_right ? pn.right_child_idx : pn.left_child_idx;
		if (next == from_node) {
			if (go_right)
				pn.right_child_idx = to_node;
			else
				pn.left_child_idx = to_node;
			return trienode_set(dt_handle, p, &pn);
		}
		x = next;
		if (trienode_get(dt_handle, x, &xn) != 0) return -1;
	}
	return -1;
}

int p_remove_key(VfsDtHandle* dt_handle, const char* key_to_remove) {
	if (!dt_handle || !key_to_remove) return -1;

	int head = p_get_head();
	int grandparent = head;
	int parent = head;
	int current = trienode_get_right(dt_handle, head);

	while (trienode_get_bindex(dt_handle, parent) < trienode_get_bindex(dt_handle, current)) {
		grandparent = parent;
		parent = current;
		current = bit_get(key_to_remove, trienode_get_bindex(dt_handle, parent)) ? trienode_get_right(dt_handle, parent)
																				 : trienode_get_left(dt_handle, parent);
	}
	if (p_compare_keys(dt_handle, key_to_remove, current) != 1) {
		return -1;
	}

	if (parent == head) {
		unsigned int kidx = trienode_get_kindex(dt_handle, current);
		if (kidx > 0) fnode_free(dt_handle, kidx);
		trienode_recover(dt_handle, current);
		p_init_head(dt_handle);
		return 0;
	}

	VfsTrieNode par_node;
	if (trienode_get(dt_handle, parent, &par_node) != 0) return -1;
	const int sibling = (par_node.left_child_idx == current) ? par_node.right_child_idx : par_node.left_child_idx;

	VfsTrieNode gp_node;
	if (trienode_get(dt_handle, grandparent, &gp_node) != 0) return -1;
	if (gp_node.left_child_idx == parent) {
		trienode_set_left(dt_handle, grandparent, sibling);
	}
	else {
		trienode_set_right(dt_handle, grandparent, sibling);
	}

	VfsTrieNode cur_node;
	if (trienode_get(dt_handle, current, &cur_node) != 0) return -1;
	const unsigned int ck = static_cast<unsigned int>(cur_node.k_index) & 0x7FFFFFFF;
	if (ck > 0) fnode_free(dt_handle, ck);

	if (current != parent) {
		char moved_key[4096];
		moved_key[0] = '\0';
		const unsigned int pk = static_cast<unsigned int>(par_node.k_index) & 0x7FFFFFFF;
		if (par_node.k_index < 0) fnode_extract_key(dt_handle, pk, moved_key);

		cur_node.k_index = par_node.k_index;
		cur_node.nt_idx = par_node.nt_idx;
		if (trienode_set(dt_handle, current, &cur_node) != 0) return -1;

		if (p_relink_upward(dt_handle, moved_key, parent, current) != 0) {
			vfs_errno = VFS_ERR_DT_INTERNAL;
			return -1;
		}
	}
	else {
		cur_node.k_index = 0;
		cur_node.nt_idx = 0;
		trienode_set(dt_handle, current, &cur_node);
	}

	if (sibling != parent) {
		trienode_recover(dt_handle, parent);
	}
	else {
		VfsTrieNode pbuf;
		if (trienode_get(dt_handle, parent, &pbuf) != 0) return -1;
		pbuf.left_child_idx = parent;
		pbuf.right_child_idx = parent;
		trienode_set(dt_handle, parent, &pbuf);
	}

	return 0;
}

char* find_prefix(const char* pattern, char* output_buffer) {
	if (!pattern || !output_buffer) {
		if (output_buffer) *output_buffer = '\0';
		return output_buffer;
	}

	const char* p = pattern;
	char* q = output_buffer;

	while (*p != '\0') {
		if (*p == '*' || *p == '?' || *p == '[') {
			break;
		}
		*q++ = *p++;
	}
	*q = '\0';
	return output_buffer;
}

int vfs_pmatch(const char* pattern, const char* string_to_test, int flags) {
	const char* p = pattern;
	const char* s = string_to_test;
	char p_char_current_iter;

	if ((flags & ~(VFS_FNM_PATHNAME | VFS_FNM_NOESCAPE | VFS_FNM_PERIOD)) != 0) {
		return -1;
	}

	while (true) {
		p_char_current_iter = *p++;

		switch (p_char_current_iter) {
		case '\0': {
			return (*s == '\0') ? 0 : 1;
		}
		case '*': {
			const char* p_after_wildcards = p;
			const char* s_after_qmarks = s;

			if ((flags & VFS_FNM_PERIOD) && *s_after_qmarks == '.' && s_after_qmarks == string_to_test) {
				return 1;
			}

			while (*p_after_wildcards == '*' || *p_after_wildcards == '?') {
				if (*p_after_wildcards == '?') {
					if (*s_after_qmarks == '\0') return 1;
					if ((flags & VFS_FNM_PATHNAME) && *s_after_qmarks == '/') return 1;
					if ((flags & VFS_FNM_PERIOD) && *s_after_qmarks == '.' && s_after_qmarks == string_to_test)
						return 1;
					s_after_qmarks++;
				}
				p_after_wildcards++;
			}

			if (*p_after_wildcards == '\0') {
				if (flags & VFS_FNM_PATHNAME) {
					return (strchr(s_after_qmarks, '/') == nullptr) ? 0 : 1;
				}
				return 0;
			}

			for (const char* s_scan = s_after_qmarks; *s_scan != '\0'; ++s_scan) {
				if ((flags & VFS_FNM_PATHNAME) && *s_scan == '/') {
					return 1;
				}

				if (vfs_pmatch(p_after_wildcards, s_scan, flags & ~VFS_FNM_PERIOD) == 0) {
					return 0;
				}
			}

			return vfs_pmatch(p_after_wildcards, s_after_qmarks, flags & ~VFS_FNM_PERIOD);
		}
		case '?': {
			if (*s == '\0') return 1;
			if ((flags & VFS_FNM_PATHNAME) && *s == '/') return 1;
			if ((flags & VFS_FNM_PERIOD) && *s == '.' && s == string_to_test) return 1;
			s++;
			break;
		}
		case '[': {
			if (*s == '\0') return 1;
			if ((flags & VFS_FNM_PATHNAME) && *s == '/') return 1;
			if ((flags & VFS_FNM_PERIOD) && *s == '.' && s == string_to_test) return 1;

			bool matched_in_class = false;
			bool negated_class = false;
			const char* p_class_start = p;

			if ((*p == '!' || *p == '^') && p[1] != ']') {
				negated_class = true;
				p++;
				p_class_start++;
			}

			char char_s_current = *s;
			char char_s_lower =
				(char_s_current >= 'A' && char_s_current <= 'Z') ? (char_s_current + 32) : char_s_current;
			char prev_class_char = '\0';

			while (true) {
				char char_p_class = *p++;
				if (char_p_class == '\0') return 1;

				if (char_p_class == ']' && p > p_class_start + 1) {
					break;
				}

				if (prev_class_char != '\0' && char_p_class == '-' && *p != ']' && *p != '\0') {
					char range_end = *p++;
					if (!(flags & VFS_FNM_NOESCAPE) && range_end == '\\') {
						range_end = *p++;
						if (range_end == '\0') return 1;
					}
					if (range_end == '\0') return 1;

					char low =
						(prev_class_char >= 'A' && prev_class_char <= 'Z') ? prev_class_char + 32 : prev_class_char;
					char high = (range_end >= 'A' && range_end <= 'Z') ? range_end + 32 : range_end;
					if (low > high) std::swap(low, high);
					if (char_s_lower >= low && char_s_lower <= high) {
						matched_in_class = true;
					}
					prev_class_char = '\0';
				}
				else {
					char pcl = (char_p_class >= 'A' && char_p_class <= 'Z') ? char_p_class + 32 : char_p_class;
					if (pcl == char_s_lower) {
						matched_in_class = true;
					}
					prev_class_char = pcl;
				}

				if (matched_in_class && !negated_class) break;
			}

			if (matched_in_class || negated_class) {
				while (*(p - 1) != '\0') {
					if (*(p - 1) == ']' && p > p_class_start + 1) break;
					if (!(flags & VFS_FNM_NOESCAPE) && *p == '\\') p++;
					p++;
				}
				if (*(p - 1) != ']') return 1;
			}

			if (negated_class == matched_in_class) return 1;

			s++;
			break;
		}
		case '\\': {
			if (!(flags & VFS_FNM_NOESCAPE)) {
				p_char_current_iter = *p++;
				if (p_char_current_iter == '\0') return 1;
			}
		}
			[[fallthrough]];
		default: {
			char char_s_current = *s;
			if (char_s_current == '\0') return 1;

			char p_compare = (p_char_current_iter >= 'A' && p_char_current_iter <= 'Z') ? (p_char_current_iter + 32)
																						: p_char_current_iter;
			char s_compare = (char_s_current >= 'A' && char_s_current <= 'Z') ? (char_s_current + 32) : char_s_current;

			if (p_compare != s_compare) {
				return 1;
			}
			s++;
			break;
		}
		}
	}
}

int p_node_iterate(VfsDtHandle* dt_handle, int current_trienode_idx, int parent_b_index, const char* glob_pattern,
	int pmatch_flags, int (*callback)(VfsDtHandle*, char*, int, void*), void* callback_context) {
	if (!dt_handle || current_trienode_idx < 0) return 1;

	VfsTrieNode current_node_data;
	if (trienode_get(dt_handle, current_trienode_idx, &current_node_data) != 0) {
		return 1;
	}

	const int current_node_b_idx = current_node_data.b_index;
	if (current_node_b_idx > parent_b_index) {
		const int left_child = current_node_data.left_child_idx;
		const int right_child = current_node_data.right_child_idx;

		if (!p_node_iterate(dt_handle, left_child, current_node_b_idx, glob_pattern, pmatch_flags, callback,
				callback_context)) {
			return 0;
		}
		if (!p_node_iterate(dt_handle, right_child, current_node_b_idx, glob_pattern, pmatch_flags, callback,
				callback_context)) {
			return 0;
		}
		return 1;
	}
	else {
		if (current_trienode_idx == 0) {
			return 1;
		}

		if (current_node_data.k_index >= 0) {
			return 1;
		}

		unsigned int keynode_start_idx = current_node_data.k_index & 0x7FFFFFFF;
		if (current_node_data.k_index == static_cast<int>(0x80000000)) keynode_start_idx = 0;

		if (fnode_extract_key(dt_handle, keynode_start_idx, vfs_glob_key_buffer) < 0) {
			return 1;
		}

		if (vfs_pmatch(glob_pattern, vfs_glob_key_buffer, pmatch_flags) == 0) {
			if (callback) {
				return callback(dt_handle, vfs_glob_key_buffer, current_trienode_idx, callback_context);
			}
		}
		return 1;
	}
}

static int __vfs_glob_sort_callback(const void* a, const void* b) {
	return strcmp(*(const char**)a, *(const char**)b);
}

static int __vfs_glob_processing_callback(VfsDtHandle* dt_handle, char* matched_filename, int trienode_idx,
	void* context) {
	if (!context) return 0;

	VfsGlobResults* results = static_cast<VfsGlobResults*>(context);

	size_t current_capacity = 0;

	char** new_pathv = static_cast<char**>(realloc(results->gl_pathv, (results->gl_pathc + 1) * sizeof(char*)));
	if (!new_pathv) {
		if (results->gl_pathv) {
		}
		results->internal_callback_error_flag = 1;
		return 0;
	}
	results->gl_pathv = new_pathv;

	results->gl_pathv[results->gl_pathc] = static_cast<char*>(malloc(strlen(matched_filename) + 1));
	if (!results->gl_pathv[results->gl_pathc]) {
		results->internal_callback_error_flag = 1;
		return 0;
	}
	strcpy(results->gl_pathv[results->gl_pathc], matched_filename);
	results->gl_pathc++;

	return 1;
}

int vfs_dt_filename_add(VfsDtHandle* dt_handle, const char* filename_to_add) {
	if (!dt_handle || !filename_to_add) return -1;
	return p_insert_key(dt_handle, filename_to_add, 0);
}

int vfs_dt_filename_delete(VfsDtHandle* dt_handle, const char* filename_to_delete) {
	if (!dt_handle || !filename_to_delete) return -1;
	return p_remove_key(dt_handle, filename_to_delete);
}

int vfs_dt_filename_lookup(VfsDtHandle* dt_handle, const char* filename_to_lookup) {
	if (!dt_handle || !filename_to_lookup) return -1;
	return p_lookup_key(dt_handle, filename_to_lookup);
}

int vfs_dt_filename_get_nt_index(VfsDtHandle* dt_handle, int trienode_idx) {
	if (!dt_handle || trienode_idx < 0) {
		return -1;
	}
	return trienode_get_nt(dt_handle, trienode_idx);
}

int vfs_dt_filename_set_nt_index(VfsDtHandle* dt_handle, int trienode_idx, int new_nt_idx) {
	if (!dt_handle || trienode_idx < 0) {
		return -1;
	}
	return trienode_set_nt(dt_handle, trienode_idx, new_nt_idx);
}

int vfs_dt_filename_get_name(VfsDtHandle* dt_handle, int trienode_idx, char* output_name_buffer) {
	if (!dt_handle || trienode_idx < 0 || !output_name_buffer) {
		if (output_name_buffer) *output_name_buffer = '\0';
		return -1;
	}
	unsigned int keynode_start_idx = trienode_get_kindex(dt_handle, trienode_idx);
	return fnode_extract_key(dt_handle, keynode_start_idx, output_name_buffer);
}

int vfs_dt_filename_glob(VfsDtHandle* dt_handle, const char* pattern, int flags,
	int (*errfunc)(const char* epath, int eerrno), VfsGlobResults* glob_results_output) {
	if (!dt_handle || !pattern || !glob_results_output) {
		return 3;
	}

	if (!(flags & VFS_FNM_APPEND)) {
		if (glob_results_output->gl_pathv) {
			for (size_t i = 0; i < glob_results_output->gl_pathc; ++i) {
				if (glob_results_output->gl_pathv[i]) free(glob_results_output->gl_pathv[i]);
			}
			free(glob_results_output->gl_pathv);
		}
		glob_results_output->gl_pathc = 0;
		glob_results_output->gl_pathv = nullptr;
		if (!(flags & VFS_FNM_DOOFFS)) {
			glob_results_output->gl_offs = 0;
		}
		if (glob_results_output->gl_offs > 0) {
			glob_results_output->gl_pathv = static_cast<char**>(malloc(glob_results_output->gl_offs * sizeof(char*)));
			if (!glob_results_output->gl_pathv) return 2;
			for (size_t i = 0; i < glob_results_output->gl_offs; ++i) {
				glob_results_output->gl_pathv[i] = nullptr;
			}
		}
	}
	glob_results_output->internal_callback_error_flag = 0;

	char prefix[256];
	find_prefix(pattern, prefix);
	int start_node_for_iteration = p_lookup_key_n(dt_handle, prefix, 8 * static_cast<int>(strlen(prefix)));
	int initial_parent_b_index = trienode_get_bindex(dt_handle, start_node_for_iteration) - 1;

	void* callback_context = glob_results_output;

	size_t matches_before_iterate = glob_results_output->gl_pathc;
	p_node_iterate(dt_handle, start_node_for_iteration, initial_parent_b_index, pattern, flags,
		__vfs_glob_processing_callback, callback_context);

	if (glob_results_output->internal_callback_error_flag) {
		return 2;
	}

	if (glob_results_output->gl_pathc == matches_before_iterate) {
		if (flags & VFS_FNM_NOCHECK) {
			char** new_pathv = static_cast<char**>(
				realloc(glob_results_output->gl_pathv, (glob_results_output->gl_pathc + 1) * sizeof(char*)));
			if (!new_pathv) return 2;
			glob_results_output->gl_pathv = new_pathv;
			glob_results_output->gl_pathv[glob_results_output->gl_pathc] =
				static_cast<char*>(malloc(strlen(pattern) + 1));
			if (!glob_results_output->gl_pathv[glob_results_output->gl_pathc]) return 2;
			strcpy(glob_results_output->gl_pathv[glob_results_output->gl_pathc], pattern);
			glob_results_output->gl_pathc++;
		}
		else {
			return 1;
		}
	}

	if (!(flags & VFS_FNM_NOSORT)) {
		if (glob_results_output->gl_pathc > 1) {
			if (glob_results_output->gl_pathc > 0) {
				qsort(glob_results_output->gl_pathv + glob_results_output->gl_offs, glob_results_output->gl_pathc,
					sizeof(char*), __vfs_glob_sort_callback);
			}
		}
	}
	return 0;
}

VfsDtHandle* vfs_dt_create(VfsIioFile* iio_file, int tn_chan_blocks, int kn_chan_blocks) {
	if (!iio_file) return nullptr;

	VfsDtHandle* dt_handle = static_cast<VfsDtHandle*>(malloc(sizeof(VfsDtHandle)));
	if (!dt_handle) return nullptr;

	dt_handle->iio_file = iio_file;

	int tn_blocks = (tn_chan_blocks == 0) ? 1 : tn_chan_blocks;
	dt_handle->trienode_channel_id = vfs_iio_allocate_channel(iio_file, tn_blocks);
	if (dt_handle->trienode_channel_id < 0) {
		free(dt_handle);
		return nullptr;
	}

	int kn_blocks = (kn_chan_blocks == 0) ? 2 : kn_chan_blocks;
	dt_handle->keynode_channel_id = vfs_iio_allocate_channel(iio_file, kn_blocks);
	if (dt_handle->keynode_channel_id < 0) {
		free(dt_handle);
		return nullptr;
	}

	dt_handle->next_free_trienode_idx = 1;
	dt_handle->next_free_keynode_idx = 1;

	if (p_init_head(dt_handle) < 0) {
		free(dt_handle);
		return nullptr;
	}
	return dt_handle;
}

VfsDtHandle* vfs_dt_open(VfsIioFile* iio_file, int trienode_channel_id, int keynode_channel_id) {
	if (!iio_file) return nullptr;

	VfsDtHandle* dt_handle = static_cast<VfsDtHandle*>(malloc(sizeof(VfsDtHandle)));
	if (!dt_handle) return nullptr;

	dt_handle->iio_file = iio_file;
	dt_handle->trienode_channel_id = trienode_channel_id;
	dt_handle->keynode_channel_id = keynode_channel_id;

	dt_handle->next_free_trienode_idx = trienode_find_first_free(dt_handle, 1);
	dt_handle->next_free_keynode_idx = keynode_find_first_free(dt_handle, 1);

	if (dt_handle->next_free_trienode_idx <= 0 || dt_handle->next_free_keynode_idx <= 0) {
		free(dt_handle);
		return nullptr;
	}

	return dt_handle;
}

int vfs_dt_close(VfsDtHandle* dt_handle) {
	if (dt_handle) {
		free(dt_handle);
	}
	return 0;
}

int vfs_dt_destroy(VfsDtHandle* dt_handle) {
	vfs_dt_close(dt_handle);
	return 0;
}

int allocate_file_descriptor(VfsHandle* handle) {
	if (!handle) return -1;

	VfsOpenFileHandle* new_fh = static_cast<VfsOpenFileHandle*>(malloc(sizeof(VfsOpenFileHandle)));
	if (!new_fh) {
		vfs_errno = VFS_ERR_NO_MEMORY;
		return -1;
	}
	memset(new_fh, 0, sizeof(VfsOpenFileHandle));
	new_fh->in_use = false;

	int fd = -1;
	if (handle->open_files_array) {
		for (int i = 0; i < handle->open_files_array_capacity; ++i) {
			if (handle->open_files_array[i] == nullptr) {
				fd = i;
				break;
			}
		}
	}

	if (fd == -1) {
		if (!handle->open_files_array) {
			handle->open_files_array_capacity = 32;
			handle->open_files_array = static_cast<VfsOpenFileHandle**>(
				malloc(sizeof(VfsOpenFileHandle*) * handle->open_files_array_capacity));
			if (!handle->open_files_array) {
				free(new_fh);
				vfs_errno = VFS_ERR_NO_MEMORY;
				return -1;
			}
			memset(handle->open_files_array, 0, sizeof(VfsOpenFileHandle*) * handle->open_files_array_capacity);
			fd = 0;
		}
		else {
			int new_capacity = handle->open_files_array_capacity * 2;
			VfsOpenFileHandle** new_array = static_cast<VfsOpenFileHandle**>(
				realloc(handle->open_files_array, sizeof(VfsOpenFileHandle*) * new_capacity));
			if (!new_array) {
				free(new_fh);
				vfs_errno = VFS_ERR_NO_MEMORY;
				return -1;
			}
			handle->open_files_array = new_array;
			memset(handle->open_files_array + handle->open_files_array_capacity, 0,
				sizeof(VfsOpenFileHandle*) * (new_capacity - handle->open_files_array_capacity));
			fd = handle->open_files_array_capacity;
			handle->open_files_array_capacity = new_capacity;
		}
	}
	handle->open_files_array[fd] = new_fh;
	return fd;
}

int deallocate_file_descriptor(VfsHandle* handle, int fd) {
	if (!handle) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return VFS_ERR_INVALID_HANDLE;
	}

	if (!handle->open_files_array || fd < 0 || fd >= handle->open_files_array_capacity) {
		vfs_errno = VFS_ERR_FILE_CLOSED;
		return VFS_ERR_FILE_CLOSED;
	}

	VfsOpenFileHandle* fh = handle->open_files_array[fd];
	if (!fh) {
		vfs_errno = VFS_ERR_FILE_CLOSED;
		return VFS_ERR_FILE_CLOSED;
	}

	if (fh->in_use) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return VFS_ERR_FILE_NOT_FOUND;
	}

	free(fh);
	handle->open_files_array[fd] = nullptr;
	return 0;
}

void file_truncate(VfsHandle* handle, VfsOpenFileHandle* fh, int new_size) {
	if (!handle || !fh || new_size < 0) return;

	int num_blocks_to_keep = (new_size + 512 - 1) / 512;
	if (new_size == 0) num_blocks_to_keep = 0;

	if (num_blocks_to_keep == 0) {
		if (fh->fat_chain_start_idx > 0) {
			vfs_fat_destroy_chain(handle->fat_handle, fh->fat_chain_start_idx);
			int block_to_become_eoc = fh->fat_chain_start_idx;
			if (num_blocks_to_keep > 0) {
				block_to_become_eoc =
					vfs_fat_chain_get_nth(handle->fat_handle, fh->fat_chain_start_idx, num_blocks_to_keep - 1);
			}
			if (block_to_become_eoc > 0) {
				vfs_fat_chain_truncate(handle->fat_handle, block_to_become_eoc);
			}
			else if (num_blocks_to_keep == 0 && fh->fat_chain_start_idx > 0) {
				vfs_fat_chain_truncate(handle->fat_handle, fh->fat_chain_start_idx);
			}
			else if (num_blocks_to_keep > 0 && block_to_become_eoc <= 0) {
			}
		}
	}
	else {
		int last_block_to_keep_idx =
			vfs_fat_chain_get_nth(handle->fat_handle, fh->fat_chain_start_idx, num_blocks_to_keep - 1);
		if (last_block_to_keep_idx > 0) {
			vfs_fat_chain_truncate(handle->fat_handle, last_block_to_keep_idx);
		}
		else {
		}
	}

	vfs_nt_node_set_size(handle->nt_handle, fh->nt_node_idx, new_size);
}

static int file_grow(VfsHandle* handle, VfsOpenFileHandle* fh, int new_size, int zero_until) {
	if (!handle || !fh) return -1;

	const int old_size = vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx);
	if (old_size < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}
	if (new_size <= old_size) return 0;

	const int zero_end = (zero_until > new_size) ? new_size : zero_until;
	const int old_num_blocks = nblocks(old_size);
	const int needed_num_blocks = nblocks(new_size);

	char zeros[512];
	memset(zeros, 0, sizeof(zeros));

	if (zero_end > old_size && old_num_blocks > 0 && fh->fat_chain_start_idx > 0) {
		const int tail_block_seq = old_num_blocks - 1;
		const int tail_covered_end = old_num_blocks * 512;
		const int tail_zero_end = (zero_end < tail_covered_end) ? zero_end : tail_covered_end;
		const int tail_off = old_size - tail_block_seq * 512;
		if (tail_zero_end > old_size && tail_off >= 0 && tail_off < 512) {
			const int blk = vfs_fat_chain_get_nth(handle->fat_handle, fh->fat_chain_start_idx, tail_block_seq);
			if (blk > 0) {
				char buf[512];
				if (vfs_data_read(handle->data_handle, blk, buf) == 0) {
					memset(buf + tail_off, 0, static_cast<size_t>(tail_zero_end - tail_block_seq * 512) - tail_off);
					vfs_data_write(handle->data_handle, blk, buf);
				}
			}
		}
	}

	int fat_chain_iter = fh->fat_chain_start_idx;
	if (old_num_blocks > 0 && fat_chain_iter > 0) {
		fat_chain_iter = find_last_in_chain(handle->fat_handle, fh->fat_chain_start_idx);
	}
	else if (old_num_blocks == 0 && fat_chain_iter <= 0) {
		fat_chain_iter = vfs_fat_create_chain(handle->fat_handle);
		if (fat_chain_iter <= 0) {
			vfs_errno = VFS_ERR_FAT_ALLOC;
			return -1;
		}
		fh->fat_chain_start_idx = fat_chain_iter;
		vfs_nt_node_set_chain(handle->nt_handle, fh->nt_node_idx, fat_chain_iter);
	}
	else if (old_num_blocks == 0 && fat_chain_iter > 0) {
		fat_chain_iter = find_last_in_chain(handle->fat_handle, fh->fat_chain_start_idx);
	}

	for (int i = old_num_blocks; i < needed_num_blocks; ++i) {
		const int new_block = vfs_fat_chain_extend(handle->fat_handle, fat_chain_iter);
		if (new_block <= 0) {
			vfs_errno = VFS_ERR_FAT_ALLOC;
			return -1;
		}
		if (fat_chain_iter <= 0 && i == old_num_blocks) {
			fh->fat_chain_start_idx = new_block;
			vfs_nt_node_set_chain(handle->nt_handle, fh->nt_node_idx, new_block);
		}
		if (i * 512 < zero_end) {
			vfs_data_write(handle->data_handle, new_block, zeros);
		}
		fat_chain_iter = new_block;
	}

	vfs_nt_node_set_size(handle->nt_handle, fh->nt_node_idx, new_size);
	return 0;
}

int vfs_file_exists(VfsHandle* handle, const char* filename) {
	if (!handle || !handle->dt_handle || !filename) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return 0;
	}
	return (vfs_dt_filename_lookup(handle->dt_handle, filename) >= 0);
}

int vfs_file_open(VfsHandle* handle, const char* filename, int open_flags) {
	if (!handle || !filename) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return -1;
	}

	int dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, filename);
	if (dt_node_idx < 0) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return -1;
	}

	int fd = allocate_file_descriptor(handle);
	if (fd < 0) {
		return -1;
	}

	VfsOpenFileHandle* fh = handle->open_files_array[fd];
	fh->open_mode_flags = open_flags;
	fh->in_use = true;
	fh->dt_trienode_idx = dt_node_idx;

	fh->nt_node_idx = vfs_dt_filename_get_nt_index(handle->dt_handle, dt_node_idx);
	if (fh->nt_node_idx < 0) {
		deallocate_file_descriptor(handle, fd);
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	VfsNode temp_nt_node;
	if (vfs_nt_get_node(handle->nt_handle, fh->nt_node_idx, &temp_nt_node) < 0) {
		deallocate_file_descriptor(handle, fd);
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}
	fh->fat_chain_start_idx = temp_nt_node.fat_chain_start_idx;
	fh->current_byte_offset = 0;

	bool has_write_access = (open_flags & VFS_OPEN_ACCESS_MODE_MASK) == VFS_OPEN_WRITE_ONLY ||
		(open_flags & VFS_OPEN_ACCESS_MODE_MASK) == VFS_OPEN_READ_WRITE;

	if ((open_flags & VFS_OPEN_TRUNCATE) && has_write_access) {
		file_truncate(handle, fh, 0);
	}

	handle->num_currently_open_files++;
	return fd;
}

int vfs_file_create(VfsHandle* handle, const char* filename) {
	if (!handle || !filename) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return -1;
	}

	int fd = -1;
	VfsOpenFileHandle* fh = nullptr;
	int dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, filename);

	if (dt_node_idx >= 0) {
		fd = vfs_file_open(handle, filename, VFS_DEFAULT_FILE_MODE | VFS_OPEN_TRUNCATE);
		if (fd < 0) {
			return -1;
		}
	}
	else {
		fd = allocate_file_descriptor(handle);
		if (fd < 0) {
			vfs_errno = VFS_ERR_NO_MEMORY;
			return -1;
		}
		fh = handle->open_files_array[fd];
		fh->in_use = true;
		fh->open_mode_flags = VFS_DEFAULT_FILE_MODE;
		fh->current_byte_offset = 0;

		fh->nt_node_idx = vfs_nt_allocate_node(handle->nt_handle);
		if (fh->nt_node_idx < 0) {
			deallocate_file_descriptor(handle, fd);
			vfs_errno = VFS_ERR_NT_ALLOC;
			return -1;
		}

		fh->fat_chain_start_idx = vfs_fat_create_chain(handle->fat_handle);
		if (fh->fat_chain_start_idx <= 0) {
			vfs_nt_refcount_decr(handle->nt_handle, fh->nt_node_idx);
			deallocate_file_descriptor(handle, fd);
			vfs_errno = VFS_ERR_FAT_ALLOC;
			return -1;
		}
		vfs_nt_node_set_chain(handle->nt_handle, fh->nt_node_idx, fh->fat_chain_start_idx);
		vfs_nt_node_set_size(handle->nt_handle, fh->nt_node_idx, 0);

		dt_node_idx = vfs_dt_filename_add(handle->dt_handle, filename);
		if (dt_node_idx < 0) {
			vfs_fat_destroy_chain(handle->fat_handle, fh->fat_chain_start_idx);
			vfs_nt_refcount_decr(handle->nt_handle, fh->nt_node_idx);
			deallocate_file_descriptor(handle, fd);
			vfs_errno = VFS_ERR_DT_INTERNAL;
			return -1;
		}
		if (vfs_dt_filename_set_nt_index(handle->dt_handle, dt_node_idx, fh->nt_node_idx) < 0) {
			vfs_errno = VFS_ERR_DT_ALLOC;
			deallocate_file_descriptor(handle, fd);
			return -1;
		}
		fh->dt_trienode_idx = dt_node_idx;

		handle->num_currently_open_files++;
	}
	return fd;
}

int vfs_file_close(VfsHandle* handle, int fd) {
	if (!handle) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return -1;
	}
	if (!handle->open_files_array || fd < 0 || fd >= handle->open_files_array_capacity ||
		handle->open_files_array[fd] == nullptr) {
		vfs_errno = VFS_ERR_INVALID_FD;
		return -1;
	}

	VfsOpenFileHandle* fh = handle->open_files_array[fd];
	fh->in_use = false;
	if (deallocate_file_descriptor(handle, fd) != 0) {
		return -1;
	}
	handle->num_currently_open_files--;
	return 0;
}

int vfs_file_lseek(VfsHandle* handle, int fd, int offset, int whence) {
	if (!handle || !handle->open_files_array || fd < 0 || fd >= handle->open_files_array_capacity ||
		!handle->open_files_array[fd]) {
		vfs_errno = VFS_ERR_INVALID_FD;
		return -1;
	}
	VfsOpenFileHandle* fh = handle->open_files_array[fd];

	int current_file_size = vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx);
	if (current_file_size < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	int new_pos;
	if (whence == 0) {
		new_pos = offset;
	}
	else if (whence == 1) {
		new_pos = fh->current_byte_offset + offset;
	}
	else if (whence == 2) {
		new_pos = current_file_size + offset;
	}
	else {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return -1;
	}

	if (new_pos < 0) new_pos = 0;

	fh->current_byte_offset = new_pos;

	if (new_pos > current_file_size) {
		if (file_grow(handle, fh, new_pos, new_pos) != 0) return -1;
	}
	return 0;
}

int vfs_file_read(VfsHandle* handle, int fd, void* buffer, int bytes_to_read) {
	if (!handle) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return -1;
	}
	if (fd < 0 || fd >= handle->open_files_array_capacity || !handle->open_files_array ||
		!handle->open_files_array[fd]) {
		vfs_errno = VFS_ERR_INVALID_FD;
		return -1;
	}
	if (!buffer || bytes_to_read < 0) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return -1;
	}
	if (bytes_to_read == 0) {
		return 0;
	}

	VfsOpenFileHandle* fh = handle->open_files_array[fd];

	int file_size = vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx);
	if (file_size < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	int effective_bytes_to_read = bytes_to_read;
	if (fh->current_byte_offset >= file_size) {
		return 0;
	}
	if (fh->current_byte_offset + effective_bytes_to_read > file_size) {
		effective_bytes_to_read = file_size - fh->current_byte_offset;
	}
	if (effective_bytes_to_read <= 0) return 0;

	char temp_block_buffer[512];
	int total_bytes_read = 0;
	char* current_user_buffer_ptr = static_cast<char*>(buffer);
	int remaining_bytes = effective_bytes_to_read;

	int current_block_seq_in_file = blockno(fh->current_byte_offset);
	int actual_fat_block_idx =
		vfs_fat_chain_get_nth(handle->fat_handle, fh->fat_chain_start_idx, current_block_seq_in_file);

	while (remaining_bytes > 0) {
		if (actual_fat_block_idx <= 0) {
			vfs_errno = VFS_ERR_FILE_NOT_FOUND;
			break;
		}

		int offset_in_physical_block = fh->current_byte_offset % 512;
		int bytes_to_read_from_this_vfs_block = min(remaining_bytes, 512 - offset_in_physical_block);

		if (vfs_data_read(handle->data_handle, actual_fat_block_idx, temp_block_buffer) != 0) {
			vfs_errno = VFS_ERR_DATA_FILE;
			break;
		}

		memcpy(current_user_buffer_ptr, temp_block_buffer + offset_in_physical_block,
			bytes_to_read_from_this_vfs_block);

		fh->current_byte_offset += bytes_to_read_from_this_vfs_block;
		current_user_buffer_ptr += bytes_to_read_from_this_vfs_block;
		total_bytes_read += bytes_to_read_from_this_vfs_block;
		remaining_bytes -= bytes_to_read_from_this_vfs_block;

		if (remaining_bytes > 0)
			actual_fat_block_idx = vfs_fat_chain_get_nth(handle->fat_handle, actual_fat_block_idx, 1);
	}
	return total_bytes_read;
}

int vfs_file_write(VfsHandle* handle, int fd, const void* buffer, int bytes_to_write) {
	if (!handle) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return -1;
	}
	if (fd < 0 || fd >= handle->open_files_array_capacity || !handle->open_files_array ||
		!handle->open_files_array[fd]) {
		vfs_errno = VFS_ERR_INVALID_FD;
		return -1;
	}
	if (!buffer || bytes_to_write < 0) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return -1;
	}
	if (bytes_to_write == 0) {
		return 0;
	}

	VfsOpenFileHandle* fh = handle->open_files_array[fd];

	if (fh->open_mode_flags & VFS_OPEN_APPEND) {
		int current_size = vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx);
		if (current_size < 0) {
			vfs_errno = VFS_ERR_INVALID_NODE;
			return -1;
		}
		if (vfs_file_lseek(handle, fd, current_size, 0) != 0) {
			return -1;
		}
	}

	const int target_end_size = fh->current_byte_offset + bytes_to_write;
	const int current_file_size = vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx);
	if (target_end_size > current_file_size) {
		if (file_grow(handle, fh, target_end_size, fh->current_byte_offset) != 0) return -1;
	}

	char temp_block_buffer[512];
	int total_bytes_written = 0;
	const char* current_user_buffer_ptr = static_cast<const char*>(buffer);
	int remaining_bytes = bytes_to_write;

	int fat_block_idx =
		vfs_fat_chain_get_nth(handle->fat_handle, fh->fat_chain_start_idx, blockno(fh->current_byte_offset));

	while (remaining_bytes > 0) {
		if (fat_block_idx <= 0) {
			vfs_errno = VFS_ERR_FAT_ALLOC;
			break;
		}

		int offset_in_block = fh->current_byte_offset % 512;
		int bytes_this_block = min(remaining_bytes, 512 - offset_in_block);
		bool full_overwrite = (offset_in_block == 0 && bytes_this_block == 512);

		if (!full_overwrite) {
			if (vfs_data_read(handle->data_handle, fat_block_idx, temp_block_buffer) != 0) {
				vfs_errno = VFS_ERR_DATA_FILE;
				break;
			}
		}

		memcpy(temp_block_buffer + offset_in_block, current_user_buffer_ptr, bytes_this_block);

		if (vfs_data_write(handle->data_handle, fat_block_idx, temp_block_buffer) != 0) {
			vfs_errno = VFS_ERR_DATA_FILE;
			break;
		}

		fh->current_byte_offset += bytes_this_block;
		current_user_buffer_ptr += bytes_this_block;
		total_bytes_written += bytes_this_block;
		remaining_bytes -= bytes_this_block;

		if (remaining_bytes > 0) fat_block_idx = vfs_fat_chain_get_nth(handle->fat_handle, fat_block_idx, 1);
	}

	if (fh->current_byte_offset > vfs_nt_node_get_size(handle->nt_handle, fh->nt_node_idx)) {
		vfs_nt_node_set_size(handle->nt_handle, fh->nt_node_idx, fh->current_byte_offset);
	}

	return total_bytes_written;
}

int vfs_file_inc_refcount(VfsHandle* handle, const char* filename) {
	if (!handle || !handle->dt_handle || !handle->nt_handle || !filename) {
		vfs_errno = (handle ? VFS_ERR_INVALID_PARAM : VFS_ERR_INVALID_HANDLE);
		return -1;
	}
	int dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, filename);
	if (dt_node_idx < 0) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return -1;
	}
	int nt_idx = vfs_dt_filename_get_nt_index(handle->dt_handle, dt_node_idx);
	if (nt_idx < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}
	vfs_nt_refcount_incr(handle->nt_handle, nt_idx);
	return 0;
}

int vfs_file_dec_refcount(VfsHandle* handle, const char* filename) {
	if (!handle || !handle->dt_handle || !handle->nt_handle || !handle->fat_handle || !filename) {
		vfs_errno = (handle ? VFS_ERR_INVALID_PARAM : VFS_ERR_INVALID_HANDLE);
		return -1;
	}
	int dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, filename);
	if (dt_node_idx < 0) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return -1;
	}
	int nt_idx = vfs_dt_filename_get_nt_index(handle->dt_handle, dt_node_idx);
	if (nt_idx < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	int fat_chain_start = vfs_nt_node_get_chain(handle->nt_handle, nt_idx);
	if (vfs_nt_refcount_decr(handle->nt_handle, nt_idx) == 1) {
		if (vfs_dt_filename_delete(handle->dt_handle, filename) < 0) {
			vfs_errno = VFS_ERR_DT_INTERNAL;
		}
		if (fat_chain_start > 0) {
			if (vfs_fat_destroy_chain(handle->fat_handle, fat_chain_start) < 0) {
				vfs_errno = VFS_ERR_FAT_INIT;
			}
		}
	}
	return 0;
}

int vfs_file_link(VfsHandle* handle, const char* old_name, const char* new_name) {
	if (!handle || !handle->dt_handle || !handle->nt_handle || !old_name || !new_name) {
		vfs_errno = (handle ? VFS_ERR_INVALID_PARAM : VFS_ERR_INVALID_HANDLE);
		return -1;
	}
	int old_dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, old_name);
	if (old_dt_node_idx < 0) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return -1;
	}
	if (vfs_dt_filename_lookup(handle->dt_handle, new_name) >= 0) {
		vfs_errno = VFS_ERR_FILE_EXISTS;
		return -1;
	}

	int nt_idx = vfs_dt_filename_get_nt_index(handle->dt_handle, old_dt_node_idx);
	if (nt_idx < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	vfs_nt_refcount_incr(handle->nt_handle, nt_idx);

	int new_dt_node_idx = vfs_dt_filename_add(handle->dt_handle, new_name);
	if (new_dt_node_idx < 0) {
		vfs_nt_refcount_decr(handle->nt_handle, nt_idx);
		vfs_errno = VFS_ERR_FILE_EXISTS;
		return -1;
	}
	if (vfs_dt_filename_set_nt_index(handle->dt_handle, new_dt_node_idx, nt_idx) < 0) {
		vfs_nt_refcount_decr(handle->nt_handle, nt_idx);
		vfs_dt_filename_delete(handle->dt_handle, new_name);
		vfs_errno = VFS_ERR_DT_INTERNAL;
		return -1;
	}
	return 0;
}

int vfs_file_unlink(VfsHandle* handle, const char* filename_to_unlink) {
	if (!handle || !handle->dt_handle || !handle->nt_handle || !handle->fat_handle || !filename_to_unlink) {
		vfs_errno = (handle ? VFS_ERR_INVALID_PARAM : VFS_ERR_INVALID_HANDLE);
		return -1;
	}
	int dt_node_idx = vfs_dt_filename_lookup(handle->dt_handle, filename_to_unlink);
	if (dt_node_idx < 0) {
		vfs_errno = VFS_ERR_FILE_NOT_FOUND;
		return -1;
	}
	int nt_idx = vfs_dt_filename_get_nt_index(handle->dt_handle, dt_node_idx);
	if (nt_idx < 0) {
		vfs_errno = VFS_ERR_INVALID_NODE;
		return -1;
	}

	int fat_chain_start = vfs_nt_node_get_chain(handle->nt_handle, nt_idx);

	if (vfs_dt_filename_delete(handle->dt_handle, filename_to_unlink) < 0) {
		vfs_errno = VFS_ERR_DT_INTERNAL;
		return -1;
	}

	if (vfs_nt_refcount_decr(handle->nt_handle, nt_idx) == 1) {
		if (fat_chain_start > 0) {
			if (vfs_fat_destroy_chain(handle->fat_handle, fat_chain_start) < 0) {
				vfs_errno = VFS_ERR_FAT_INIT;
			}
		}
	}
	return 0;
}

int vfs_glob(VfsHandle* handle, const char* pattern, int flags, int (*errfunc)(const char* epath, int eerrno),
	VfsGlobResults* glob_results) {
	if (!handle || !handle->dt_handle || !pattern || !glob_results) {
		vfs_errno = (handle ? VFS_ERR_INVALID_PARAM : VFS_ERR_INVALID_HANDLE);
		return 3;
	}

	return vfs_dt_filename_glob(handle->dt_handle, pattern, flags, errfunc, glob_results);
}

void vfs_glob_free(VfsGlobResults* glob_results) {
	if (!glob_results) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return;
	}
	if (glob_results->gl_pathv) {
		for (size_t i = 0; i < glob_results->gl_pathc; ++i) {
			if (glob_results->gl_pathv[glob_results->gl_offs + i]) {
				free(glob_results->gl_pathv[glob_results->gl_offs + i]);
			}
		}
		free(glob_results->gl_pathv);
	}
	glob_results->gl_pathc = 0;
	glob_results->gl_pathv = nullptr;
	glob_results->gl_offs = 0;
}

int vfs_exists(const char* base_vfs_name) {
	if (!base_vfs_name) return 0;
	char paki_filename[256];
	char pak_filename[256];

	strcpy(paki_filename, base_vfs_name);
	strcat(paki_filename, ".paki");

	strcpy(pak_filename, base_vfs_name);
	strcat(pak_filename, ".pak");

	return file_exists(paki_filename) > 0 && file_exists(pak_filename) > 0;
}

VfsHandle* vfs_start(const char* base_vfs_name, int access_mode) {
	if (!base_vfs_name) {
		vfs_errno = VFS_ERR_INVALID_PARAM;
		return nullptr;
	}

	VfsHandle* handle = static_cast<VfsHandle*>(malloc(sizeof(VfsHandle)));
	if (!handle) {
		vfs_errno = VFS_ERR_NO_MEMORY;
		return nullptr;
	}
	memset(handle, 0, sizeof(VfsHandle));

	vfs_iio_IOMODE = access_mode;
	vfs_data_IOMODE = access_mode;

	if (!lock_check(base_vfs_name, access_mode)) {
		vfs_errno = VFS_ERR_LOCKED;
		free(handle);
		return nullptr;
	}

	char paki_filename[256];
	char pak_filename[256];
	strcpy(paki_filename, base_vfs_name);
	strcat(paki_filename, ".paki");
	strcpy(pak_filename, base_vfs_name);
	strcat(pak_filename, ".pak");

	bool paki_exists = file_exists(paki_filename);
	bool pak_exists = file_exists(pak_filename);

	handle->base_vfs_name_for_lock = static_cast<char*>(malloc(strlen(base_vfs_name) + 1));
	if (!handle->base_vfs_name_for_lock) {
		vfs_errno = VFS_ERR_NO_MEMORY;
		free(handle);
		return nullptr;
	}
	strcpy(handle->base_vfs_name_for_lock, base_vfs_name);

	if (paki_exists && pak_exists) {
		handle->iio_handle = vfs_iio_open(paki_filename);
		if (!handle->iio_handle) {
			vfs_errno = VFS_ERR_IIO_FILE;
			goto cleanup_error;
		}

		handle->data_handle = vfs_data_open(pak_filename);
		if (!handle->data_handle) {
			vfs_errno = VFS_ERR_DATA_FILE;
			goto cleanup_error;
		}

		handle->dt_handle = vfs_dt_open(handle->iio_handle, 0, 1);
		if (!handle->dt_handle) {
			vfs_errno = VFS_ERR_DT_INIT;
			goto cleanup_error;
		}

		handle->nt_handle = vfs_nt_open(handle->iio_handle, 2);
		if (!handle->nt_handle) {
			vfs_errno = VFS_ERR_NT_INIT;
			goto cleanup_error;
		}

		handle->fat_handle = vfs_fat_open(handle->iio_handle, 3);
		if (!handle->fat_handle) {
			vfs_errno = VFS_ERR_FAT_INIT;
			goto cleanup_error;
		}
	}
	else {
		if (access_mode == 1 && (!paki_exists || !pak_exists)) {
			vfs_errno = VFS_ERR_IIO_FILE;
			goto cleanup_error;
		}
		if (paki_exists != pak_exists) {
			vfs_errno = VFS_ERR_IIO_FILE;
			goto cleanup_error;
		}

		handle->iio_handle = vfs_iio_create(paki_filename);
		if (!handle->iio_handle) {
			vfs_errno = VFS_ERR_IIO_FILE;
			goto cleanup_error;
		}

		handle->data_handle = vfs_data_create(pak_filename);
		if (!handle->data_handle) {
			vfs_errno = VFS_ERR_DATA_FILE;
			goto cleanup_error;
		}

		handle->dt_handle = vfs_dt_create(handle->iio_handle, 4, 8);
		if (!handle->dt_handle) {
			vfs_errno = VFS_ERR_DT_ALLOC;
			goto cleanup_error;
		}

		handle->nt_handle = vfs_nt_create(handle->iio_handle, 1);
		if (!handle->nt_handle) {
			vfs_errno = VFS_ERR_NT_ALLOC;
			goto cleanup_error;
		}

		handle->fat_handle = vfs_fat_create(handle->iio_handle, 4);
		if (!handle->fat_handle) {
			vfs_errno = VFS_ERR_FAT_ALLOC;
			goto cleanup_error;
		}
	}
	return handle;

cleanup_error:
	if (handle) {
		if (handle->fat_handle) vfs_fat_close(handle->fat_handle);
		if (handle->nt_handle) vfs_nt_close(handle->nt_handle);
		if (handle->dt_handle) vfs_dt_close(handle->dt_handle);
		if (handle->data_handle) {
			if (!pak_exists)
				vfs_data_destroy(handle->data_handle);
			else
				vfs_data_close(handle->data_handle);
		}
		if (handle->iio_handle) {
			if (!paki_exists)
				vfs_iio_destroy(handle->iio_handle);
			else
				vfs_iio_close(handle->iio_handle);
		}
		if (handle->base_vfs_name_for_lock) free(handle->base_vfs_name_for_lock);
		free(handle);
	}
	return nullptr;
}

void vfs_end(VfsHandle* handle, int destroy_files_flag) {
	if (!handle) {
		vfs_errno = VFS_ERR_INVALID_HANDLE;
		return;
	}

	if (destroy_files_flag) {
		if (handle->dt_handle) vfs_dt_destroy(handle->dt_handle);
		if (handle->nt_handle) vfs_nt_destroy(handle->nt_handle);
		if (handle->fat_handle) vfs_fat_close(handle->fat_handle);

		if (handle->data_handle) vfs_data_destroy(handle->data_handle);
		if (handle->iio_handle) vfs_iio_destroy(handle->iio_handle);
	}
	else {
		if (handle->dt_handle) vfs_dt_close(handle->dt_handle);
		if (handle->nt_handle) vfs_nt_close(handle->nt_handle);
		if (handle->fat_handle) vfs_fat_close(handle->fat_handle);
		if (handle->data_handle) vfs_data_close(handle->data_handle);
		if (handle->iio_handle) vfs_iio_close(handle->iio_handle);
	}
	handle->dt_handle = nullptr;
	handle->nt_handle = nullptr;
	handle->fat_handle = nullptr;
	handle->data_handle = nullptr;
	handle->iio_handle = nullptr;

	if (handle->open_files_array) {
		for (int i = 0; i < handle->open_files_array_capacity; ++i) {
			if (handle->open_files_array[i]) {
				deallocate_file_descriptor(handle, i);
			}
		}
		free(handle->open_files_array);
		handle->open_files_array = nullptr;
	}
	handle->open_files_array_capacity = 0;
	handle->num_currently_open_files = 0;

	if (handle->base_vfs_name_for_lock) {
		lock_leave(handle->base_vfs_name_for_lock);
		free(handle->base_vfs_name_for_lock);
		handle->base_vfs_name_for_lock = nullptr;
	}
	free(handle);
}
