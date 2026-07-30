#pragma once

typedef struct llru_item{
	struct llru_item *next;
	struct llru_item *prev;
	uint64_t key;
}LLRU_ITEM;

typedef struct{
	LLRU_ITEM *head;
	LLRU_ITEM *tail;
	LFreeFunc free;
	uint64_t last_clear;
	int size;
	int length;
}LLRU;

LLRU *l_lru_new(LFreeFunc free);
void l_lru_free(LLRU *lru);
int l_lru_add(LLRU *lru,LLRU_ITEM *item);
LLRU_ITEM *l_lru_get(LLRU *lru,uint64_t key);

