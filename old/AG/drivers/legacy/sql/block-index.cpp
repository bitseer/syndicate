/*
   Copyright 2013 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "block-index.h"
#include <stdio.h>

BlockIndex::BlockIndex()
{
    map_mutex = new pthread_mutex_t;
    pthread_mutex_init(map_mutex, NULL);
}

block_index_entry* BlockIndex::alloc_block_index_entry()
{
    block_index_entry *blkie = new block_index_entry;
    blkie->start_row = 0;
    blkie->start_byte_offset = 0;
    blkie->end_row = 0;
    blkie->end_byte_offset = 0;
    return blkie;
}

void BlockIndex::update_block_index(string file_name, off_t block_id, block_index_entry* blkie)
{
    vector<block_index_entry*> *blk_list = NULL, *old_blk_list = NULL;
    pthread_mutex_t blk_index_mutex = PTHREAD_MUTEX_INITIALIZER;
    BlockMap::iterator itr = blk_map.find(file_name);
    MutexMap::iterator mitr = mutex_map.find(file_name);
    if (itr != blk_map.end() && mitr != mutex_map.end()) {
	//Acquire lock for this file's block index
	pthread_mutex_lock(&(mitr->second));
	blk_list = itr->second;
	blk_list->resize(blk_list->size() + 1);
	(*blk_list)[block_id] = blkie;
	//Release block index lock
	pthread_mutex_unlock(&(mitr->second));
    }
    else {
	blk_list = new vector<block_index_entry*>();
	blk_list->reserve(MAX_INDEX_SIZE);
	blk_list->resize(1);
	(*blk_list)[block_id] = blkie;
	//Acquire locks for maps...
	pthread_mutex_lock(map_mutex);
	//Check whether someone has already updated 
	//blk_map and mutex_map... if so replace them.
	if ((old_blk_list = blk_map[file_name]) != NULL) {
	    blk_map[file_name] = blk_list;
	    delete old_blk_list;
	    mutex_map[file_name] = blk_index_mutex;
	}
	else {
	    blk_map[file_name] = blk_list;
	    mutex_map[file_name] = blk_index_mutex;
	}
	//Release locks for maps...
	pthread_mutex_unlock(map_mutex);
    }
}

const block_index_entry* BlockIndex::get_block(string file_name, off_t block_id)
{
    if (block_id < 0)
	return NULL;
    vector<block_index_entry*> *blk_list = NULL;
    BlockMap::iterator itr = blk_map.find(file_name);
    if (itr != blk_map.end()) {
	blk_list = itr->second;
	if ((ssize_t)blk_list->size() <= block_id)
	    return NULL;
	if (blk_list)
	    return (*blk_list)[block_id];
	else
	    return NULL;
    }
    else 
	return NULL;
}

const block_index_entry* BlockIndex::get_last_block(string file_name, off_t *block_id)
{
    vector<block_index_entry*> *blk_list = NULL;
    BlockMap::iterator itr = blk_map.find(file_name);
    if (itr != blk_map.end()) {
	blk_list = itr->second;
	if (blk_list) {
	    block_index_entry *blkie =  blk_list->back();
	    *block_id = blk_list->size() - 1;
	    return blkie;
	}
	return NULL;
    }
    else 
	return NULL;
}


void BlockIndex::invalidate_entry(string file_name) {
    vector<block_index_entry*> *blk_list = NULL;
    BlockMap::iterator itr = blk_map.find(file_name);
    MutexMap::iterator mitr = mutex_map.find(file_name);
    if (itr != blk_map.end() && mitr != mutex_map.end()) {
	pthread_mutex_lock(map_mutex);
	//Acquire lock for this file's block index
	pthread_mutex_lock(&(mitr->second));
	blk_list = itr->second;
	free_block_index(blk_list);
	blk_list->resize(blk_list->size() - 1);
	//Release block index lock
	pthread_mutex_unlock(&(mitr->second));
	pthread_mutex_unlock(map_mutex);
    }
}

void BlockIndex::free_block_index(vector<block_index_entry*> *blk_list) {
    vector<block_index_entry*>::iterator itr;
    for (itr=blk_list->begin(); itr!=blk_list->end(); itr++) {
	delete *itr;
    }
    delete blk_list;
}

