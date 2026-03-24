/* fat.c - Read/write access to the FAT

   Copyright (C) 1993 Werner Almesberger <werner.almesberger@lrc.di.epfl.ch>
   Copyright (C) 1998 Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
   Copyright (C) 2008-2014 Daniel Baumann <mail@daniel-baumann.ch>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.

   The complete text of the GNU General Public License
   can be found in /usr/share/common-licenses/GPL-3 file.
*/

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "fsck.fat.h"
#include "io.h"
#include "check.h"
#include "fat.h"

#define CLUSTER_MAX   (3900000U)  //保证内存允许的情况下的最大cluster的数量

/*
 * 256G 超过此值时才真正启用 -L / -B。
 */
#define FSCK_CLUSTER_OPTIMIZE_THRESHOLD  (5700000U)

/*
 * FAT_LAZY_LOAD 宏在 fsck.fat.h 中定义，可在该文件中统一开启或关闭。
 * 开启后，FAT32 大容量卡（簇数 > CLUSTER_MAX）改用单窗口懒加载模式，
 * 节省约 4.8MB FAT 缓冲内存；关闭后所有路径与原始实现完全相同。
 */

static int read_fat_mode = 0;

/**
 * Fetch the FAT entry for a specified cluster.
 *
 * @param[out]  entry	    Cluster to which cluster of interest is linked
 * @param[in]	fat	    FAT table for the partition
 * @param[in]	cluster     Cluster of interest
 * @param[in]	fs          Information from the FAT boot sectors (bits per FAT entry)
 */
void get_fat(FAT_ENTRY * entry, void *fat, uint32_t cluster, DOS_FS * fs)
{
    unsigned char *ptr;

    if (cluster > fs->data_clusters + 1) {
	die("Internal error: cluster out of range in get_fat() (%lu > %lu).",
		(unsigned long)cluster, (unsigned long)(fs->data_clusters + 1));
    }

    switch (fs->fat_bits) {
    case 12:
	ptr = &((unsigned char *)fat)[cluster * 3 / 2];
	entry->value = 0xfff & (cluster & 1 ? (ptr[0] >> 4) | (ptr[1] << 4) :
				(ptr[0] | ptr[1] << 8));
	break;
    case 16:
	entry->value = le16toh(((unsigned short *)fat)[cluster]);
	break;
    case 32:
	/* According to M$, the high 4 bits of a FAT32 entry are reserved and
	 * are not part of the cluster number. So we cut them off. */
	{
#ifdef FAT_LAZY_LOAD
	    /* 若当前处于懒加载模式且访问的是 fs->fat 窗口缓冲，
	     * 则在读取前检查 cluster 是否在窗口内，超出则换入对应段。
	     * 注意：传入非 fs->fat 的临时缓冲（如双FAT比较时）不走此分支。*/
	    if (fat == (void *)fs->fat && fs->fat_lazy) {
		if (cluster < fs->fat_win_start ||
		    cluster >= fs->fat_win_start + CLUSTER_MAX) {
		    uint32_t new_start = (cluster / CLUSTER_MAX) * CLUSTER_MAX;
		    uint32_t cnt = fs->data_clusters + 2 - new_start;
		    if (cnt > CLUSTER_MAX)
			cnt = CLUSTER_MAX;
		    /* fs_read 会自动合并 changes 队列，无需手动写回旧窗口 */
		    fs_read(fs->fat_start + (off_t)new_start * 4,
			   (int)(cnt * 4), fs->fat);
		    fs->fat_win_start = new_start;
		}
		{
		    uint32_t e = le32toh(
			((unsigned int *)fat)[cluster - fs->fat_win_start]);
		    entry->value = e & 0xfffffff;
		    entry->reserved = e >> 28;
		}
		break;
	    }
#endif /* FAT_LAZY_LOAD */
	    uint32_t e = le32toh(((unsigned int *)fat)[cluster]);
	    entry->value = e & 0xfffffff;
	    entry->reserved = e >> 28;
	}
	break;
    default:
	die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }
}

/**
 * Build a bookkeeping structure from the partition's FAT table.
 * If the partition has multiple FATs and they don't agree, try to pick a winner,
 * and queue a command to overwrite the loser.
 * One error that is fixed here is a cluster that links to something out of range.
 *
 * @param[inout]    fs      Information about the filesystem
 */

/*获取read_fat的模式 by lvqiao*/
void set_read_fat_mode(int mode)
{
    if (mode)
    {
        printf("set_read_fat_mode 1\n");
        read_fat_mode = 1;
    }
}

void read_fat(DOS_FS * fs)
{
    int eff_size, alloc_size;
    uint32_t i;
    void *first, *second = NULL;
    int first_ok, second_ok;
    uint32_t total_num_clusters;

    /* Clean up from previous pass */
    if (fs->fat)
	free(fs->fat);
    if (fs->cluster_owner)
	free(fs->cluster_owner);
#ifdef CLUSTER_OWNER_BITMAP
    if (fs->cluster_bitmap)
	free(fs->cluster_bitmap);
    fs->cluster_bitmap = NULL;
#endif
    fs->fat = NULL;
    fs->cluster_owner = NULL;

    total_num_clusters = fs->data_clusters + 2;
    eff_size = (total_num_clusters * fs->fat_bits + 7) / 8ULL;

#if defined(FAT_LAZY_LOAD) || defined(CLUSTER_OWNER_BITMAP)
    if (total_num_clusters <= FSCK_CLUSTER_OPTIMIZE_THRESHOLD) {
# ifdef FAT_LAZY_LOAD
	if (fs->fat_lazy_enable) {
	    printf("Note: total clusters %u <= %u — ignoring -L, using full FAT.\n",
		   (unsigned)total_num_clusters,
		   (unsigned)FSCK_CLUSTER_OPTIMIZE_THRESHOLD);
	    fs->fat_lazy_enable = 0;
	}
# endif
# ifdef CLUSTER_OWNER_BITMAP
	if (fs->cluster_owner_mode == 1) {
	    printf("Note: total clusters %u <= %u — ignoring -B, using pointer cluster_owner.\n",
		   (unsigned)total_num_clusters,
		   (unsigned)FSCK_CLUSTER_OPTIMIZE_THRESHOLD);
	    fs->cluster_owner_mode = 0;
	}
# endif
    }
#endif

    /* -s（read_fat_mode）：仅比较总簇数与 FSCK_CLUSTER_OPTIMIZE_THRESHOLD，与 -L/-B/-S 无关 */
    if (read_fat_mode && total_num_clusters > FSCK_CLUSTER_OPTIMIZE_THRESHOLD) {
	printf("clusters:%u exceeds FSCK_CLUSTER_OPTIMIZE_THRESHOLD (%u), exit (-s).\n",
	       (unsigned)total_num_clusters,
	       (unsigned)FSCK_CLUSTER_OPTIMIZE_THRESHOLD);
	exit(0);
    }

#ifdef FAT_LAZY_LOAD
    /*
     * FAT32 大容量卡懒加载路径：
     * - fat_lazy_enable == 0: 全表 FAT（小卷或已按阈值关闭 -L）
     * - fat_lazy_enable == 1（-L）: 仅当 fat_bits==32 且簇数 > CLUSTER_MAX 时启用窗口
     * fs->fat 只保留单个 CLUSTER_MAX 大小的滑动窗口，按需换入。
     */
    if (fs->fat_lazy_enable == 1 && fs->fat_bits == 32 && total_num_clusters > (uint32_t)CLUSTER_MAX) {
	uint32_t seg, cnt;
	void *seg_buf = NULL;
	int differ = 0;

	printf("FAT32 lazy-load enabled: clusters=%u, window=%d\n",
	       total_num_clusters, CLUSTER_MAX);

	/* 只分配单窗口缓冲区：CLUSTER_MAX 个 FAT32 表项 = 约 2.4MB */
	alloc_size = CLUSTER_MAX * 4;
	first = alloc(alloc_size);

	if (fs->nfats > 1) {
	    FAT_ENTRY first_media, second_media;

	    seg_buf = alloc(alloc_size);
	    first_ok = second_ok = 1;

	    /* --- 分段比较双 FAT --- */
	    for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
		cnt = total_num_clusters - seg;
		if (cnt > (uint32_t)CLUSTER_MAX)
		    cnt = CLUSTER_MAX;
		fs_read(fs->fat_start + (off_t)seg * 4,
			(int)(cnt * 4), first);
		fs_read(fs->fat_start + fs->fat_size + (off_t)seg * 4,
			(int)(cnt * 4), seg_buf);
		if (seg == 0) {
		    /* 媒体字节仅在第一段检查（含 FAT 类型标记） */
		    get_fat(&first_media, first, 0, fs);
		    get_fat(&second_media, seg_buf, 0, fs);
		    first_ok = (first_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);
		    second_ok = (second_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);
		}
		if (memcmp(first, seg_buf, cnt * 4) != 0)
		    differ = 1;
	    }

	    /* --- 处理双 FAT 差异（逻辑同原始，改为逐段写入） --- */
	    if (differ) {
		if (first_ok && !second_ok) {
		    printf("FATs differ - using first FAT.\n");
		    for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
			cnt = total_num_clusters - seg;
			if (cnt > (uint32_t)CLUSTER_MAX) cnt = CLUSTER_MAX;
			fs_read(fs->fat_start + (off_t)seg * 4,
				(int)(cnt * 4), first);
			fs_write(fs->fat_start + fs->fat_size + (off_t)seg * 4,
				 (int)(cnt * 4), first);
		    }
		}
		if (!first_ok && second_ok) {
		    printf("FATs differ - using second FAT.\n");
		    for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
			cnt = total_num_clusters - seg;
			if (cnt > (uint32_t)CLUSTER_MAX) cnt = CLUSTER_MAX;
			fs_read(fs->fat_start + fs->fat_size + (off_t)seg * 4,
				(int)(cnt * 4), seg_buf);
			fs_write(fs->fat_start + (off_t)seg * 4,
				 (int)(cnt * 4), seg_buf);
		    }
		}
		if (first_ok && second_ok) {
		    if (interactive) {
			printf("FATs differ but appear to be intact. Use which FAT ?\n"
			       "1) Use first FAT\n2) Use second FAT\n");
			if (get_key("12", "?") == '1') {
			    for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
				cnt = total_num_clusters - seg;
				if (cnt > (uint32_t)CLUSTER_MAX) cnt = CLUSTER_MAX;
				fs_read(fs->fat_start + (off_t)seg * 4,
					(int)(cnt * 4), first);
				fs_write(fs->fat_start + fs->fat_size + (off_t)seg * 4,
					 (int)(cnt * 4), first);
			    }
			} else {
			    for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
				cnt = total_num_clusters - seg;
				if (cnt > (uint32_t)CLUSTER_MAX) cnt = CLUSTER_MAX;
				fs_read(fs->fat_start + fs->fat_size + (off_t)seg * 4,
					(int)(cnt * 4), seg_buf);
				fs_write(fs->fat_start + (off_t)seg * 4,
					 (int)(cnt * 4), seg_buf);
			    }
			}
		    } else {
			printf("FATs differ but appear to be intact. Using first "
			       "FAT.\n");
			for (seg = 0; seg < total_num_clusters; seg += CLUSTER_MAX) {
			    cnt = total_num_clusters - seg;
			    if (cnt > (uint32_t)CLUSTER_MAX) cnt = CLUSTER_MAX;
			    fs_read(fs->fat_start + (off_t)seg * 4,
				    (int)(cnt * 4), first);
			    fs_write(fs->fat_start + fs->fat_size + (off_t)seg * 4,
				     (int)(cnt * 4), first);
			}
		    }
		}
		if (!first_ok && !second_ok) {
		    free(first);
		    free(seg_buf);
		    printf("Both FATs appear to be corrupt. Giving up.\n");
		    exit(1);
		}
	    }
	    free(seg_buf);
	}

	/* 加载第一个窗口到 fs->fat。
	 * fs_read 会自动合并 changes 队列，若上面做了 FAT 修复写入，
	 * 此处读取的数据已经反映了修复结果。*/
	cnt = (total_num_clusters < (uint32_t)CLUSTER_MAX)
	      ? total_num_clusters : (uint32_t)CLUSTER_MAX;
	fs_read(fs->fat_start, (int)(cnt * 4), first);

	fs->fat          = (unsigned char *)first;
	fs->fat_win_start = 0;
	fs->fat_lazy      = 1;

	printf("total_num_clusters:%u, lazy alloc_size:%d\n",
	       total_num_clusters, alloc_size);

#ifdef CLUSTER_OWNER_BITMAP
	if (fs->cluster_owner_mode == 1) {
	    /* Bitmap mode: 1 bit per cluster */
	    size_t bitmap_bytes = (total_num_clusters + 7) / 8;
	    fs->cluster_bitmap = alloc(bitmap_bytes);
	    memset(fs->cluster_bitmap, 0, bitmap_bytes);
	    printf("Cluster owner bitmap allocated: %zu bytes (%.1f MB)\n",
		   bitmap_bytes, (double)bitmap_bytes / (1024 * 1024));
	} else
#endif
	{
	    /* Normal mode: pointer array */
	    fs->cluster_owner = alloc(total_num_clusters * sizeof(DOS_FILE *));
	    memset(fs->cluster_owner, 0, total_num_clusters * sizeof(DOS_FILE *));
	}

	/* 越界链修复循环：get_fat / set_fat 内部会自动换入所需窗口 */
	for (i = 2; i < fs->data_clusters + 2; i++) {
	    FAT_ENTRY curEntry;
	    get_fat(&curEntry, fs->fat, i, fs);
	    if (curEntry.value == 1) {
		printf("Cluster %ld out of range (1). Setting to EOF.\n",
		       (long)(i - 2));
		set_fat(fs, i, -1);
	    }
	    if (curEntry.value >= fs->data_clusters + 2 &&
		(curEntry.value < FAT_MIN_BAD(fs))) {
		printf("Cluster %ld out of range (%ld > %ld). Setting to EOF.\n",
		       (long)(i - 2), (long)curEntry.value,
		       (long)(fs->data_clusters + 2 - 1));
		set_fat(fs, i, -1);
	    }
	}
	return;  /* 懒加载路径完成，跳过后续的全量加载逻辑 */
    }
#endif /* FAT_LAZY_LOAD */
        
    if (fs->fat_bits != 12)
	    alloc_size = eff_size;
    else
	    /* round up to an even number of FAT entries to avoid special
	     * casing the last entry in get_fat() */
	    alloc_size = (total_num_clusters * 12 + 23) / 24 * 3;
    printf("total_num_clusters:%d, alloc_size:%d\n", total_num_clusters, alloc_size);
    
    first = alloc(alloc_size);
    fs_read(fs->fat_start, eff_size, first);
    if (fs->nfats > 1) {
	second = alloc(alloc_size);
	fs_read(fs->fat_start + fs->fat_size, eff_size, second);
    }
    if (second && memcmp(first, second, eff_size) != 0) {
	FAT_ENTRY first_media, second_media;
	get_fat(&first_media, first, 0, fs);
	get_fat(&second_media, second, 0, fs);
	first_ok = (first_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);
	second_ok = (second_media.value & FAT_EXTD(fs)) == FAT_EXTD(fs);
	if (first_ok && !second_ok) {
	    printf("FATs differ - using first FAT.\n");
	    fs_write(fs->fat_start + fs->fat_size, eff_size, first);
	}
	if (!first_ok && second_ok) {
	    printf("FATs differ - using second FAT.\n");
	    fs_write(fs->fat_start, eff_size, second);
	    memcpy(first, second, eff_size);
	}
	if (first_ok && second_ok) {
	    if (interactive) {
		printf("FATs differ but appear to be intact. Use which FAT ?\n"
		       "1) Use first FAT\n2) Use second FAT\n");
		if (get_key("12", "?") == '1') {
		    fs_write(fs->fat_start + fs->fat_size, eff_size, first);
		} else {
		    fs_write(fs->fat_start, eff_size, second);
		    memcpy(first, second, eff_size);
		}
	    } else {
		printf("FATs differ but appear to be intact. Using first "
		       "FAT.\n");
		fs_write(fs->fat_start + fs->fat_size, eff_size, first);
	    }
	}
	if (!first_ok && !second_ok) {
	    printf("Both FATs appear to be corrupt. Giving up.\n");
	    exit(1);
	}
    }
    if (second) {
	free(second);
    }
    fs->fat = (unsigned char *)first;
#ifdef FAT_LAZY_LOAD
    fs->fat_lazy = 0;
#endif

#ifdef CLUSTER_OWNER_BITMAP
    if (fs->cluster_owner_mode == 1) {
	/* Bitmap mode: 1 bit per cluster */
	size_t bitmap_bytes = (total_num_clusters + 7) / 8;
	fs->cluster_bitmap = alloc(bitmap_bytes);
	memset(fs->cluster_bitmap, 0, bitmap_bytes);
	printf("Cluster owner bitmap allocated: %zu bytes (%.1f MB)\n",
	       bitmap_bytes, (double)bitmap_bytes / (1024 * 1024));
    } else
#endif
    {
	/* Normal mode: pointer array */
	fs->cluster_owner = alloc(total_num_clusters * sizeof(DOS_FILE *));
	memset(fs->cluster_owner, 0, (total_num_clusters * sizeof(DOS_FILE *)));
    }

    /* Truncate any cluster chains that link to something out of range */
    for (i = 2; i < fs->data_clusters + 2; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);
	if (curEntry.value == 1) {
	    printf("Cluster %ld out of range (1). Setting to EOF.\n",
		   (long)(i - 2));
	    set_fat(fs, i, -1);
	}
	if (curEntry.value >= fs->data_clusters + 2 &&
	    (curEntry.value < FAT_MIN_BAD(fs))) {
	    printf("Cluster %ld out of range (%ld > %ld). Setting to EOF.\n",
		   (long)(i - 2), (long)curEntry.value,
		   (long)(fs->data_clusters + 2 - 1));
	    set_fat(fs, i, -1);
	}
    }
}


/**
 * Update the FAT entry for a specified cluster
 * (i.e., change the cluster it links to).
 * Queue a command to write out this change.
 *
 * @param[in,out]   fs          Information about the filesystem
 * @param[in]	    cluster     Cluster to change
 * @param[in]       new	        Cluster to link to
 *				Special values:
 *				   0 == free cluster
 *				  -1 == end-of-chain
 *				  -2 == bad cluster
 */
void set_fat(DOS_FS * fs, uint32_t cluster, int32_t new)
{
    unsigned char *data = NULL;
    int size;
    off_t offs;

    if (cluster > fs->data_clusters + 1) {
	die("Internal error: cluster out of range in set_fat() (%lu > %lu).",
		(unsigned long)cluster, (unsigned long)(fs->data_clusters + 1));
    }

    if (new == -1)
	new = FAT_EOF(fs);
    else if ((long)new == -2)
	new = FAT_BAD(fs);
    else if (new > fs->data_clusters + 1) {
	die("Internal error: new cluster out of range in set_fat() (%lu > %lu).",
		(unsigned long)new, (unsigned long)(fs->data_clusters + 1));
    }

    switch (fs->fat_bits) {
    case 12:
	data = fs->fat + cluster * 3 / 2;
	offs = fs->fat_start + cluster * 3 / 2;
	if (cluster & 1) {
	    FAT_ENTRY prevEntry;
	    get_fat(&prevEntry, fs->fat, cluster - 1, fs);
	    data[0] = ((new & 0xf) << 4) | (prevEntry.value >> 8);
	    data[1] = new >> 4;
	} else {
	    FAT_ENTRY subseqEntry;
	    if (cluster != fs->data_clusters + 1)
		get_fat(&subseqEntry, fs->fat, cluster + 1, fs);
	    else
		subseqEntry.value = 0;
	    data[0] = new & 0xff;
	    data[1] = (new >> 8) | ((0xff & subseqEntry.value) << 4);
	}
	size = 2;
	break;
    case 16:
	data = fs->fat + cluster * 2;
	offs = fs->fat_start + cluster * 2;
	*(unsigned short *)data = htole16(new);
	size = 2;
	break;
    case 32:
	{
	    FAT_ENTRY curEntry;
	    /* get_fat 在懒加载模式下会自动将 cluster 所在段换入窗口 */
	    get_fat(&curEntry, fs->fat, cluster, fs);

#ifdef FAT_LAZY_LOAD
	    /* 懒加载模式下：get_fat 已保证 cluster 在当前窗口内，
	     * data 指针使用窗口内偏移，避免越界写 */
	    if (fs->fat_lazy)
		data = fs->fat + (cluster - fs->fat_win_start) * 4;
	    else
#endif /* FAT_LAZY_LOAD */
	    data = fs->fat + cluster * 4;
	    offs = fs->fat_start + cluster * 4;
	    /* According to M$, the high 4 bits of a FAT32 entry are reserved and
	     * are not part of the cluster number. So we never touch them. */
	    *(uint32_t *)data = htole32((new & 0xfffffff) |
					     (curEntry.reserved << 28));
	    size = 4;
	}
	break;
    default:
	die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }
    fs_write(offs, size, data);
    if (fs->nfats > 1) {
	fs_write(offs + fs->fat_size, size, data);
    }
}

int bad_cluster(DOS_FS * fs, uint32_t cluster)
{
    FAT_ENTRY curEntry;
    get_fat(&curEntry, fs->fat, cluster, fs);

    return FAT_IS_BAD(fs, curEntry.value);
}

/**
 * Get the cluster to which the specified cluster is linked.
 * If the linked cluster is marked bad, abort.
 *
 * @param[in]   fs          Information about the filesystem
 * @param[in]	cluster     Cluster to follow
 *
 * @return  -1              'cluster' is at the end of the chain
 * @return  Other values    Next cluster in this chain
 */
uint32_t next_cluster(DOS_FS * fs, uint32_t cluster)
{
    uint32_t value;
    FAT_ENTRY curEntry;

    get_fat(&curEntry, fs->fat, cluster, fs);

    value = curEntry.value;
    if (FAT_IS_BAD(fs, value))
	die("Internal error: next_cluster on bad cluster");
    return FAT_IS_EOF(fs, value) ? -1 : value;
}

off_t cluster_start(DOS_FS * fs, uint32_t cluster)
{
    return fs->data_start + ((off_t)cluster - 2) * (uint64_t)fs->cluster_size;
}

/**
 * Update internal bookkeeping to show that the specified cluster belongs
 * to the specified dentry.
 *
 * @param[in,out]   fs          Information about the filesystem
 * @param[in]	    cluster     Cluster being assigned
 * @param[in]	    owner       Information on dentry that owns this cluster
 *                              (may be NULL)
 */
void set_owner(DOS_FS * fs, uint32_t cluster, DOS_FILE * owner)
{
#ifdef CLUSTER_OWNER_BITMAP
    if (fs->cluster_owner_mode == 1) {
	/* Bitmap mode: 1 bit per cluster */
	if (fs->cluster_bitmap == NULL)
	    die("Internal error: attempt to set owner in non-existent bitmap");
	
	uint32_t byte_idx = cluster / 8;
	uint8_t  bit_mask = 1 << (cluster % 8);
	
	if (owner) {
	    fs->cluster_bitmap[byte_idx] |= bit_mask;  /* Set bit */
	} else {
	    fs->cluster_bitmap[byte_idx] &= ~bit_mask; /* Clear bit */
	}
	return;
    }
#endif

    
    /* Normal mode: pointer array */
    if (fs->cluster_owner == NULL)
	die("Internal error: attempt to set owner in non-existent table");

    if (owner && fs->cluster_owner[cluster]
	&& (fs->cluster_owner[cluster] != owner))
	die("Internal error: attempt to change file owner");
    fs->cluster_owner[cluster] = owner;
}

DOS_FILE *get_owner(DOS_FS * fs, uint32_t cluster)
{
#ifdef CLUSTER_OWNER_BITMAP
    if (fs->cluster_owner_mode == 1) {
	/* Bitmap mode: can only return "used" or "free" */
	if (fs->cluster_bitmap == NULL)
	    return NULL;
	
	uint32_t byte_idx = cluster / 8;
	uint8_t  bit_mask = 1 << (cluster % 8);
	
	/* Return non-NULL if bit is set (cluster is used) */
	if (fs->cluster_bitmap[byte_idx] & bit_mask)
	    return (DOS_FILE *)1;  /* Non-NULL dummy value */
	else
	    return NULL;
    }
#endif

    
    /* Normal mode: return actual owner pointer */
    if (fs->cluster_owner == NULL)
	return NULL;
    else
	return fs->cluster_owner[cluster];
}

void fix_bad(DOS_FS * fs)
{
    uint32_t i;

    if (verbose)
	printf("Checking for bad clusters.\n");
    for (i = 2; i < fs->data_clusters + 2; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);

	if (!get_owner(fs, i) && !FAT_IS_BAD(fs, curEntry.value))
	    if (!fs_test(cluster_start(fs, i), fs->cluster_size)) {
		printf("Cluster %lu is unreadable.\n", (unsigned long)i);
		set_fat(fs, i, -2);
	    }
    }
}

void reclaim_free(DOS_FS * fs)
{
    int reclaimed;
    uint32_t i;

    if (verbose)
	printf("Checking for unused clusters.\n");
    reclaimed = 0;
    for (i = 2; i < fs->data_clusters + 2; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);

	if (!get_owner(fs, i) && curEntry.value &&
	    !FAT_IS_BAD(fs, curEntry.value)) {
	    set_fat(fs, i, 0);
	    reclaimed++;
	}
    }
    if (reclaimed)
	printf("Reclaimed %d unused cluster%s (%llu bytes).\n", (int)reclaimed,
	       reclaimed == 1 ? "" : "s",
	       (unsigned long long)reclaimed * fs->cluster_size);
}

/**
 * Assign the specified owner to all orphan chains (except cycles).
 * Break cross-links between orphan chains.
 *
 * @param[in,out]   fs             Information about the filesystem
 * @param[in]	    owner          dentry to be assigned ownership of orphans
 * @param[in,out]   num_refs	   For each orphan cluster [index], how many
 *				   clusters link to it.
 * @param[in]	    start_cluster  Where to start scanning for orphans
 */
static void tag_free(DOS_FS * fs, DOS_FILE * owner, uint32_t *num_refs,
		     uint32_t start_cluster)
{
    int prev;
    uint32_t i, walk;

    if (start_cluster == 0)
	start_cluster = 2;

    for (i = start_cluster; i < fs->data_clusters + 2; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);

	/* If the current entry is the head of an un-owned chain... */
	if (curEntry.value && !FAT_IS_BAD(fs, curEntry.value) &&
	    !get_owner(fs, i) && !num_refs[i]) {
	    prev = 0;
	    /* Walk the chain, claiming ownership as we go */
	    for (walk = i; walk != -1; walk = next_cluster(fs, walk)) {
		if (!get_owner(fs, walk)) {
		    set_owner(fs, walk, owner);
		} else {
		    /* We've run into cross-links between orphaned chains,
		     * or a cycle with a tail.
		     * Terminate this orphan chain (break the link)
		     */
		    set_fat(fs, prev, -1);

		    /* This is not necessary because 'walk' is owned and thus
		     * will never become the head of a chain (the only case
		     * that would matter during reclaim to files).
		     * It's easier to decrement than to prove that it's
		     * unnecessary.
		     */
		    num_refs[walk]--;
		    break;
		}
		prev = walk;
	    }
	}
    }
}

/**
 * Recover orphan chains to files, handling any cycles or cross-links.
 *
 * @param[in,out]   fs             Information about the filesystem
 */
void reclaim_file(DOS_FS * fs)
{
    DOS_FILE orphan;
    int reclaimed, files;
    int changed = 0;
    uint32_t i, next, walk;
    uint32_t *num_refs = NULL;	/* Only for orphaned clusters */
    uint32_t total_num_clusters;
#ifdef CLUSTER_OWNER_BITMAP
    /* In bitmap mode get_owner() cannot distinguish "owned by a real file"
     * from "owned by &orphan" (both return non-zero).  We therefore keep a
     * compact bitmap that remembers which clusters were unowned at the start
     * of reclaim_file so that after tag_free() marks them we can still tell
     * them apart from clusters that were file-owned all along.
     * Memory cost: ceil(total_clusters / 8) bytes  (~950 KB for 7.6 M clusters)
     */
    uint8_t *was_orphan = NULL;
#define WAS_ORPHAN_SET(idx)  (was_orphan[(idx) >> 3] |=  (1u << ((idx) & 7)))
#define WAS_ORPHAN_GET(idx)  (was_orphan[(idx) >> 3] &   (1u << ((idx) & 7)))
#endif

    if (verbose)
	printf("Reclaiming unconnected clusters.\n");

    total_num_clusters = fs->data_clusters + 2;
    num_refs = alloc(total_num_clusters * sizeof(uint32_t));
    memset(num_refs, 0, (total_num_clusters * sizeof(uint32_t)));
#ifdef CLUSTER_OWNER_BITMAP
    if (fs->cluster_owner_mode == 1) {
	was_orphan = alloc((total_num_clusters + 7) / 8);
	memset(was_orphan, 0, (total_num_clusters + 7) / 8);
    }
#endif

    /* Guarantee that all orphan chains (except cycles) end cleanly
     * with an end-of-chain mark.
     */

    for (i = 2; i < total_num_clusters; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);

	next = curEntry.value;
#ifdef CLUSTER_OWNER_BITMAP
	/* In bitmap mode, record ALL unowned non-free non-bad clusters as
	 * orphans BEFORE the inner if-block, because the inner block only
	 * covers clusters with a valid forward link (next < data_clusters+2).
	 * Clusters whose FAT value is EOC (0x0FFFFFFF) or another out-of-range
	 * non-bad value are still orphans and must be captured here so that
	 * is_orphan_head detection works correctly after tag_free().
	 */
	if (fs->cluster_owner_mode == 1 &&
	    !get_owner(fs, i) && next && !FAT_IS_BAD(fs, next))
	    WAS_ORPHAN_SET(i);
#endif
	if (!get_owner(fs, i) && next && next < fs->data_clusters + 2) {
	    /* Cluster is linked, but not owned (orphan) */
	    FAT_ENTRY nextEntry;
	    get_fat(&nextEntry, fs->fat, next, fs);

	    /* Mark it end-of-chain if it links into an owned cluster,
	     * a free cluster, or a bad cluster.
	     */
	    if (get_owner(fs, next) || !nextEntry.value ||
		FAT_IS_BAD(fs, nextEntry.value))
		set_fat(fs, i, -1);
	    else
		num_refs[next]++;
	}
    }

    /* Scan until all the orphans are accounted for,
     * and all cycles and cross-links are broken
     */
    do {
	tag_free(fs, &orphan, num_refs, changed);
	changed = 0;

	/* Any unaccounted-for orphans must be part of a cycle */
	    for (i = 2; i < total_num_clusters; i++) {
		FAT_ENTRY curEntry;
		get_fat(&curEntry, fs->fat, i, fs);

#ifdef CLUSTER_OWNER_BITMAP
		if (curEntry.value && !FAT_IS_BAD(fs, curEntry.value) &&
		!get_owner(fs, i) && curEntry.value < total_num_clusters) {
		    if (!num_refs[curEntry.value]--)
			die("Internal error: num_refs going below zero");
		    set_fat(fs, i, -1);
		changed = curEntry.value;
		printf("Broke cycle at cluster %lu in free chain.\n", (unsigned long)i);

		/* If we've created a new chain head,
		 * tag_free() can claim it
		 */
		    if (num_refs[curEntry.value] == 0)
			break;
		}
else
		if (curEntry.value && !FAT_IS_BAD(fs, curEntry.value) &&
		    !get_owner(fs, i)) {
		    if (!num_refs[curEntry.value]--)
			die("Internal error: num_refs going below zero");
		    set_fat(fs, i, -1);
		    changed = curEntry.value;
		    printf("Broke cycle at cluster %lu in free chain.\n", (unsigned long)i);

		    /* If we've created a new chain head,
		     * tag_free() can claim it
		     */
		    if (num_refs[curEntry.value] == 0)
			break;
		}
#endif
	    }
	}
	while (changed);

	/* Now we can start recovery */
	files = reclaimed = 0;
    for (i = 2; i < total_num_clusters; i++) {
#ifdef CLUSTER_OWNER_BITMAP
	/* Bitmap mode: get_owner() returns (DOS_FILE*)1 for ANY owned cluster,
	 * not just those marked by tag_free() with &orphan.  We disambiguate
	 * using was_orphan[]: a cluster is an orphan chain head iff
	 *   (a) it was unowned at the start of reclaim_file (was_orphan bit set)
	 *   (b) tag_free() has now claimed it (get_owner returns non-zero)
	 *   (c) no other orphan cluster points to it (num_refs[i] == 0, head)
	 */
	int is_orphan_head = (fs->cluster_owner_mode == 1)
	    ? (WAS_ORPHAN_GET(i) && get_owner(fs, i) && !num_refs[i])
	    : (get_owner(fs, i) == &orphan && !num_refs[i]);
#else
	int is_orphan_head = (get_owner(fs, i) == &orphan && !num_refs[i]);
#endif
	if (is_orphan_head) {
		DIR_ENT de;
		off_t offset;
		files++;
		offset = alloc_rootdir_entry(fs, &de, "FSCK%04dREC", 1);
		de.start = htole16(i & 0xffff);
		if (fs->fat_bits == 32)
		    de.starthi = htole16(i >> 16);
		for (walk = i; walk > 0 && walk != -1;
		     walk = next_cluster(fs, walk)) {
		    de.size = htole32(le32toh(de.size) + fs->cluster_size);
		    reclaimed++;
		}
		fs_write(offset, sizeof(DIR_ENT), &de);
	    }
    }
    if (reclaimed)
	printf("Reclaimed %d unused cluster%s (%llu bytes) in %d chain%s.\n",
	       reclaimed, reclaimed == 1 ? "" : "s",
	       (unsigned long long)reclaimed * fs->cluster_size, files,
	       files == 1 ? "" : "s");

    free(num_refs);
#ifdef CLUSTER_OWNER_BITMAP
    if (was_orphan)
	free(was_orphan);
#endif
}

uint32_t update_free(DOS_FS * fs)
{
    uint32_t i;
    uint32_t free = 0;
    int do_set = 0;

    for (i = 2; i < fs->data_clusters + 2; i++) {
	FAT_ENTRY curEntry;
	get_fat(&curEntry, fs->fat, i, fs);

	if (!get_owner(fs, i) && !FAT_IS_BAD(fs, curEntry.value))
	    ++free;
    }

    if (!fs->fsinfo_start)
	return free;

    if (verbose)
	printf("Checking free cluster summary.\n");
    if (fs->free_clusters != 0xFFFFFFFF) {
	if (free != fs->free_clusters) {
	    printf("Free cluster summary wrong (%ld vs. really %ld)\n",
		   (long)fs->free_clusters, (long)free);
	    if (interactive)
		printf("1) Correct\n2) Don't correct\n");
	    else
		printf("  Auto-correcting.\n");
	    if (!interactive || get_key("12", "?") == '1')
		do_set = 1;
	}
    } else {
	printf("Free cluster summary uninitialized (should be %ld)\n", (long)free);
	if (rw) {
	    if (interactive)
		printf("1) Set it\n2) Leave it uninitialized\n");
	    else
		printf("  Auto-setting.\n");
	    if (!interactive || get_key("12", "?") == '1')
		do_set = 1;
	}
    }

    if (do_set) {
	uint32_t le_free = htole32(free);
	fs->free_clusters = free;
	fs_write(fs->fsinfo_start + offsetof(struct info_sector, free_clusters),
		 sizeof(le_free), &le_free);
    }

    return free;
}
