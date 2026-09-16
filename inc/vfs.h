#ifndef VFS_H
#define VFS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define VFS_ERR_NONE 			(0)
#define VFS_ERR_IIO_FILE 		(1)
#define VFS_ERR_DATA_FILE 		(2)
#define VFS_ERR_FAT_INIT 		(3)
#define VFS_ERR_NT_INIT 		(4)
#define VFS_ERR_DT_INIT 		(5)
#define VFS_ERR_FAT_ALLOC 		(6)
#define VFS_ERR_NT_ALLOC 		(7)
#define VFS_ERR_DT_ALLOC 		(8)
#define VFS_ERR_INVALID_HANDLE 	(9)
#define VFS_ERR_FILE_CLOSED 	(10)
#define VFS_ERR_FILE_NOT_FOUND 	(11)
#define VFS_ERR_NO_OPEN_FILES 	(12)
#define VFS_ERR_INVALID_FD 		(13)
#define VFS_ERR_INVALID_NODE 	(14)
#define VFS_ERR_NO_MEMORY 		(15)
#define VFS_ERR_LOCKED 			(16)
#define VFS_ERR_FILE_EXISTS 	(17)
#define VFS_ERR_DT_INTERNAL 	(18)
#define VFS_ERR_INVALID_PARAM 	(19)

struct VfsIioCachePage {
	uintptr_t buffer_ptr_and_syncflag_storage;
	int disk_block_position;
	int last_access_time;
};

struct VfsIioCache {
	VfsIioCachePage** pages;
	int num_pages_allocated;
	int num_pages_active;
};

struct VfsIioChannel {
	int blocks_per_stripe;
	int current_size_bytes;
	int current_seek_position;
	VfsIioCache* cache_header;
};

struct VfsIioFile {
	FILE* file_handle;
	char* file_name;
	short num_channels;
	VfsIioChannel** channels;
};

struct VfsDataCacheHeader {
	size_t buffer_capacity;
	void* buffer;
	long long cache_window_start_offset;
	int is_synced_flag;
};

struct VfsDataHandle {
	FILE* file_ptr;
	char* file_name;
	VfsDataCacheHeader* cache;
};

struct VfsFatHandle {
	VfsIioFile* iio_file;
	int fat_iio_channel_id;
	int next_free_search_start_idx;

	unsigned int* free_bits;
	int free_words;
};

struct VfsNode {
	int ref_count;
	int file_size_bytes;
	int fat_chain_start_idx;
	int user_flags_or_type;
};

struct VfsNtHandle {
	VfsIioFile* iio_file;
	int nt_iio_channel_id;
	int next_free_node_search_start_idx;
};

struct VfsTrieNode {
	int nt_idx;
	int b_index;
	int k_index;
	int left_child_idx;
	int right_child_idx;
};

struct VfsKeyNode {
	int next_fragment_idx_flags;
	char key_fragment[60];
};

struct VfsDtHandle {
	VfsIioFile* iio_file;
	int trienode_channel_id;
	int keynode_channel_id;
	int next_free_trienode_idx;
	int next_free_keynode_idx;
};

struct VfsGlobResults {
	size_t gl_pathc;
	char** gl_pathv;
	size_t gl_offs;
	int internal_callback_error_flag;
};

struct VfsOpenFileHandle {
	int dt_trienode_idx;
	int nt_node_idx;
	int fat_chain_start_idx;
	int current_byte_offset;
	int open_mode_flags;
	bool in_use;
};

struct VfsHandle {
	VfsIioFile* iio_handle;
	VfsDtHandle* dt_handle;
	VfsNtHandle* nt_handle;
	VfsFatHandle* fat_handle;
	VfsDataHandle* data_handle;
	VfsOpenFileHandle** open_files_array;
	int open_files_array_capacity;
	int num_currently_open_files;
	void* unknown_ptr_or_padding1;
	char* base_vfs_name_for_lock;
};

extern int vfs_iio_BLOCK_SIZEv;
extern int vfs_iio_CLOCK;
extern int vfs_iio_IOMODE;
extern int vfs_iio_CACHE_PAGES;

extern int vfs_data_IOMODE;
extern int vfs_data_CACHE_BYTES;

extern char vfs_glob_key_buffer[4096];

extern int vfs_errno;

extern long long vfs_stat_fat_read;
extern long long vfs_stat_fat_write;
extern long long vfs_stat_fat_scan_steps;
extern long long vfs_stat_data_slide;
extern long long vfs_stat_data_read;
extern long long vfs_stat_data_write;

unsigned long get_page_size(void);
int file_exists(const char* fileName);

int bit_get(const char* byteValue, int bitIndex);
int bitfirst_different(const char* str1, const char* str2);

int nblocks(int size);
int blockno(int offset);

int lock_remove(const char* baseName);
int lock_check(const char* baseName, int accessMode);
int lock_leave(const char* baseName);

void vfs_perror(void* unusedHandle, const char* prefixMessage);
void vfs_stat_reset(void);

void* cache_page_get_buffer(VfsIioCachePage* page);
void cache_page_set_sync(VfsIioCachePage* page, int sync_status);
int cache_page_get_sync(VfsIioCachePage* page);

int header_size(VfsIioFile* file);
int read_absolute_block_n(VfsIioFile* file, int absolute_block_index, int num_blocks_to_read, void* buffer);
int write_absolute_block_n(VfsIioFile* file, int absolute_block_index, int num_blocks_to_write, const void* buffer);

int vfs_iio_blocks_per_chunk(VfsIioFile* file);
int channel_block_to_absolute_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx);
int channel_pos_to_absolute_block(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos);

VfsIioChannel* vfs_iio_get_channel(VfsIioFile* file, int channel_idx);
int vfs_iio_channel_size(VfsIioChannel* channel);
int vfs_iio_channel_blocks(VfsIioChannel* channel);

int cache_page_choose_best_to_reuse(VfsIioFile* file, int channel_idx, int exclude_page_idx);
int cache_expand(VfsIioFile* file, int channel_idx, int required_page_array_idx);
int cache_page_dump(VfsIioFile* file, int channel_idx, int page_array_idx);
void cache_pageflush(VfsIioFile* file, int channel_idx, int page_array_idx);
void cache_page_create(VfsIioFile* file, int channel_idx, int page_array_idx_to_create_at);
int cache_page_refresh(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos);
int is_in_cache(VfsIioFile* file, int channel_idx, int channel_relative_block_idx);
void cache_update(VfsIioFile* file, int channel_idx, int channel_relative_byte_pos);
int cache_create(VfsIioFile* file, int channel_idx);
int cache_destroy(VfsIioCache* cache_to_destroy);
void cacheflush(VfsIioFile* file, int channel_idx);
void flush_data(VfsIioFile* file);

int cache_read_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx, void* buffer);
unsigned int cache_read_partial_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx,
	int offset_in_block, int end_offset_in_block, void* buffer);
int cache_write_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx, const void* buffer);
int cache_write_partial_channel_block(VfsIioFile* file, int channel_idx, int channel_relative_block_idx,
	int offset_in_block, int end_offset_in_block, const void* buffer);

int write_header(VfsIioFile* file);
int read_header(VfsIioFile* file);
void auto_truncate(VfsIioFile* file);

int vfs_iio_seek(VfsIioFile* file, int channel_idx, int seek_position);
int vfs_iio_read(VfsIioFile* file, int channel_idx, void* buffer, int bytes_to_read);
int vfs_iio_write(VfsIioFile* file, int channel_idx, const void* buffer, int bytes_to_write);
int vfs_iio_channel_truncate(VfsIioFile* file, int channel_idx);

int vfs_iio_allocate_channel(VfsIioFile* file, int blocks_per_stripefor_new_channel);
VfsIioFile* vfs_iio_create(const char* fileName);
VfsIioFile* vfs_iio_open(const char* fileName);
void vfs_iio_close(VfsIioFile* file_to_close);
void vfs_iio_destroy(VfsIioFile* file_to_destroy);

int is_in_cache(VfsDataHandle* handle, long long block_index_to_check);
int cache_flush(VfsDataHandle* handle);
int cache_slide(VfsDataHandle* handle, long long new_desired_start_offset);
int cache_create(VfsDataHandle* handle);
int cache_resize(VfsDataHandle* handle, size_t new_capacity);
int cache_destroy(VfsDataHandle* handle);
int cache_get(VfsDataHandle* handle, long long file_offset_to_read_from, int num_bytes_to_read, void* output_buffer);
int cache_put(VfsDataHandle* handle, long long file_offset_to_write_to, int num_bytes_to_write,
	const void* input_buffer);

int vfs_data_read(VfsDataHandle* handle, int block_index, void* buffer);
int vfs_data_write(VfsDataHandle* handle, int block_index, const void* buffer);
int vfs_data_read_contiguous(VfsDataHandle* handle, int start_block_index, int num_blocks, void* buffer);
int vfs_data_write_contiguous(VfsDataHandle* handle, int start_block_index, int num_blocks, const void* buffer);

int vfs_data_flush_cache(VfsDataHandle* handle);
int vfs_data_set_cache_size(VfsDataHandle* handle, size_t new_size);

VfsDataHandle* vfs_data_create(const char* fileName);
VfsDataHandle* vfs_data_open(const char* fileName);
void vfs_data_close(VfsDataHandle* handle);
void vfs_data_destroy(VfsDataHandle* handle);

int node_get_value(VfsFatHandle* fat_handle, int fat_entry_index);
void node_set_value(VfsFatHandle* fat_handle, int fat_entry_index, int value_to_set);
int next_free(VfsFatHandle* fat_handle, int start_search_idx);
int find_last_in_chain(VfsFatHandle* fat_handle, int start_of_chain_idx);
int node_recover(VfsFatHandle* fat_handle, int fat_entry_idx_to_free);

void vfs_fat_chain_for_each(VfsFatHandle* fat_handle, int start_of_chain_idx, int (*callback)(VfsFatHandle*, int));
int vfs_fat_chain_get_nth(VfsFatHandle* fat_handle, int start_of_chain_idx, int n);
void vfs_fat_chain_get(VfsFatHandle* fat_handle, int start_of_chain_idx, void* output_buffer);
void vfs_fat_chain_get_first_n(VfsFatHandle* fat_handle, int start_of_chain_idx, int num_entries_to_get,
	void* output_buffer);

int vfs_fat_create_chain(VfsFatHandle* fat_handle);
int vfs_fat_destroy_chain(VfsFatHandle* fat_handle, int start_of_chain_idx);
int vfs_fat_chain_extend(VfsFatHandle* fat_handle, int chain_to_extend);
int vfs_fat_chain_truncate(VfsFatHandle* fat_handle, int entry_idx_to_become_new_eoc);
int vfs_fat_chain_shrink(VfsFatHandle* fat_handle, int start_of_chain_idx, int num_blocks_to_keep);

VfsFatHandle* vfs_fat_create(VfsIioFile* iio_file, int num_iio_blocks_for_fat_channel);
VfsFatHandle* vfs_fat_open(VfsIioFile* iio_file, int fat_iio_channel_id);
int vfs_fat_close(VfsFatHandle* fat_handle);

int node_get(VfsNtHandle* nt_handle, int node_index, VfsNode* node_buffer);
int node_set(VfsNtHandle* nt_handle, int node_index, const VfsNode* node_data);
int find_first_free(VfsNtHandle* nt_handle, int start_search_idx);
int node_recover(VfsNtHandle* nt_handle, int node_idx_to_free);

int vfs_nt_get_node(VfsNtHandle* nt_handle, int node_index, VfsNode* node_buffer);
int vfs_nt_set_node(VfsNtHandle* nt_handle, int node_index, const VfsNode* node_data);
int vfs_nt_allocate_node(VfsNtHandle* nt_handle);

int vfs_nt_node_get_size(VfsNtHandle* nt_handle, int node_index);
void vfs_nt_node_set_size(VfsNtHandle* nt_handle, int node_index, int new_size);
int vfs_nt_node_get_chain(VfsNtHandle* nt_handle, int node_index);
void vfs_nt_node_set_chain(VfsNtHandle* nt_handle, int node_index, int new_fat_chain_start_idx);

void vfs_nt_refcount_incr(VfsNtHandle* nt_handle, int node_index);
int vfs_nt_refcount_decr(VfsNtHandle* nt_handle, int node_index);

VfsNtHandle* vfs_nt_create(VfsIioFile* iio_file, int num_iio_blocks_for_nt_channel);
VfsNtHandle* vfs_nt_open(VfsIioFile* iio_file, int nt_iio_channel_id);
void vfs_nt_close(VfsNtHandle* nt_handle);
void vfs_nt_destroy(VfsNtHandle* nt_handle);

int trienode_get(VfsDtHandle* dt_handle, int tn_idx, VfsTrieNode* out_node);
int trienode_set(VfsDtHandle* dt_handle, int tn_idx, VfsTrieNode* node_to_set);
int keynode_get(VfsDtHandle* dt_handle, int kn_idx, VfsKeyNode* out_node);
int keynode_set(VfsDtHandle* dt_handle, int kn_idx, const VfsKeyNode* node_to_set);

int trienode_is_free(VfsDtHandle* dt_handle, int tn_idx);
int trienode_find_first_free(VfsDtHandle* dt_handle, int start_idx);
int trienode_clear(VfsDtHandle* dt_handle, int tn_idx);
int trienode_recover(VfsDtHandle* dt_handle, int tn_idx);
int keynode_is_free(VfsDtHandle* dt_handle, int kn_idx);
int keynode_find_first_free(VfsDtHandle* dt_handle, int start_idx);
int keynode_clear(VfsDtHandle* dt_handle, int kn_idx);

unsigned int trienode_get_left(VfsDtHandle* dt_handle, int tn_idx);
unsigned int trienode_get_right(VfsDtHandle* dt_handle, int tn_idx);
int trienode_get_bindex(VfsDtHandle* dt_handle, int tn_idx);
unsigned int trienode_get_kindex(VfsDtHandle* dt_handle, int tn_idx);
int trienode_get_nt(VfsDtHandle* dt_handle, int tn_idx);
int trienode_set_left(VfsDtHandle* dt_handle, int tn_idx, int left_child_idx);
int trienode_set_right(VfsDtHandle* dt_handle, int tn_idx, int right_child_idx);
int trienode_set_nt(VfsDtHandle* dt_handle, int tn_idx, int nt_idx);

int fnode_extract_key(VfsDtHandle* dt_handle, int start_keynode_idx, char* output_buffer);
int fnode_free(VfsDtHandle* dt_handle, int start_keynode_idx_to_free);
int fnode_allocate(VfsDtHandle* dt_handle, const char* source_string);
int node_allocate(VfsDtHandle* dt_handle, const char* key_string, int nt_idx_for_trienode, int b_index_for_trienode);
int node_copy_key(VfsDtHandle* dt_handle, int source_trienode_idx, int dest_trienode_idx);

int p_get_head();
int p_init_head(VfsDtHandle* dt_handle);
int p_compare_keys(VfsDtHandle* dt_handle, const char* key_to_compare, int external_trienode_idx);
int p_find_first_different_bit(VfsDtHandle* dt_handle, const char* key1, int trienode_idx_for_key2);
int p_lookup_key(VfsDtHandle* dt_handle, const char* key_to_lookup);
int p_lookup_key_n(VfsDtHandle* dt_handle, const char* key_to_lookup, int num_bits_to_match);
int p_insert_key(VfsDtHandle* dt_handle, const char* key_to_insert, int nt_idx);
int p_remove_key(VfsDtHandle* dt_handle, const char* key_to_remove);

char* find_prefix(const char* pattern, char* output_buffer);
int vfs_pmatch(const char* pattern, const char* string_to_test, int flags);
int p_node_iterate(VfsDtHandle* dt_handle, int current_trienode_idx, int parent_b_index, const char* glob_pattern,
	int pmatch_flags, int (*callback)(VfsDtHandle*, char*, int, void*), void* callback_context);

int vfs_dt_filename_add(VfsDtHandle* dt_handle, const char* filename_to_add);
int vfs_dt_filename_delete(VfsDtHandle* dt_handle, const char* filename_to_delete);
int vfs_dt_filename_lookup(VfsDtHandle* dt_handle, const char* filename_to_lookup);
int vfs_dt_filename_get_nt_index(VfsDtHandle* dt_handle, int trienode_idx);
int vfs_dt_filename_set_nt_index(VfsDtHandle* dt_handle, int trienode_idx, int new_nt_idx);
int vfs_dt_filename_get_name(VfsDtHandle* dt_handle, int trienode_idx, char* output_name_buffer);
int vfs_dt_filename_glob(VfsDtHandle* dt_handle, const char* pattern, int flags,
	int (*errfunc)(const char* epath, int eerrno), VfsGlobResults* glob_results_output);

VfsDtHandle* vfs_dt_create(VfsIioFile* iio_file, int tn_chan_blocks, int kn_chan_blocks);
VfsDtHandle* vfs_dt_open(VfsIioFile* iio_file, int trienode_channel_id, int keynode_channel_id);
int vfs_dt_close(VfsDtHandle* dt_handle);
int vfs_dt_destroy(VfsDtHandle* dt_handle);

int allocate_file_descriptor(VfsHandle* handle);
int deallocate_file_descriptor(VfsHandle* handle, int fd);

void file_truncate(VfsHandle* handle, VfsOpenFileHandle* fh, int new_size);

int vfs_file_exists(VfsHandle* handle, const char* filename);
int vfs_file_open(VfsHandle* handle, const char* filename, int open_flags);
int vfs_file_create(VfsHandle* handle, const char* filename);
int vfs_file_close(VfsHandle* handle, int fd);
int vfs_file_lseek(VfsHandle* handle, int fd, int offset, int whence);
int vfs_file_read(VfsHandle* handle, int fd, void* buffer, int bytes_to_read);
int vfs_file_write(VfsHandle* handle, int fd, const void* buffer, int bytes_to_write);

int vfs_file_inc_refcount(VfsHandle* handle, const char* filename);
int vfs_file_dec_refcount(VfsHandle* handle, const char* filename);
int vfs_file_link(VfsHandle* handle, const char* old_name, const char* new_name);
int vfs_file_unlink(VfsHandle* handle, const char* filename_to_unlink);

int vfs_glob(VfsHandle* handle, const char* pattern, int flags, int (*errfunc)(const char* epath, int eerrno),
	VfsGlobResults* glob_results);
void vfs_glob_free(VfsGlobResults* glob_results);

int vfs_exists(const char* base_vfs_name);
VfsHandle* vfs_start(const char* base_vfs_name, int access_mode);
void vfs_end(VfsHandle* handle, int destroy_files_flag);

#endif
