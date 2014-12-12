/*
 * Zach Abt, Soo Bin Kwon
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

/*
 * Compare the number of clusters in FAT and the size of metadata, and modify accordingly
 * This is called in FAT_scan
 */
int check_cluster_number(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb, int clusters_meta, int clusters_fat){
    uint16_t cluster = getushort(dirent->deStartCluster);
    uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;

    if(clusters_fat == clusters_meta){ 
        return 1;
    }
    
    /*
     * If the file size in the metadata (directory entry) is less than the size suggested by the cluster chain, 
     * modify the cluster chain via the FAT to make it consistent with the directory entry
     */
    else if(clusters_fat > clusters_meta){
        printf("\t*BAD:\tFile size in the metadata is smaller than the cluster chain length for the file would suggest.\n");       
                
        //modify FAT
        int i = 0;
        while (is_valid_cluster(cluster,bpb)){            
            i++;            
            uint16_t next = get_fat_entry(cluster, image_buf, bpb); 
            if(i == clusters_meta){
                set_fat_entry(cluster, (CLUST_EOFS&FAT12_MASK), image_buf, bpb);
                printf("\t\tCluster %d changed to EOF.\n",cluster);
            }else if(i > clusters_meta){
                set_fat_entry(cluster, (CLUST_FREE&FAT12_MASK), image_buf, bpb);
                printf("\t\tCluster %d freed.\n",cluster);
            }        
            cluster = next;
        }       
        
        return 0;
    }
    
    /*
     * If the directory entry file size is greater than the size indicated by the cluster chain, update the directory entry
     */
    else{
        printf("\t*BAD:\tFile size in the metadata that is larger than the cluster chain for the file would suggest.\n");
        uint32_t bytes_needed = getulong(dirent->deFileSize);
        int new_filesize = clusters_fat * cluster_size;
        printf("\t\tFile size in metadata modified from %d(%d clusters) to %d(%d clusters).\n",bytes_needed,clusters_meta,new_filesize,clusters_fat);  
        
        //update the 8-bit unsigned integers in directory entry
        dirent->deFileSize[0] =  (u_int8_t) (new_filesize % 256);
        dirent->deFileSize[1] = (u_int8_t) (new_filesize / 256);
        
        return 0;
    }
}

/*
 * Scan through valid cluster chain, look for bad clusters, and check cluster numbers
 * This is called in scan_dirent
 */

void FAT_scan(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb, int option, int arr[]){
    uint16_t cluster = getushort(dirent->deStartCluster);
    uint32_t bytes_needed = getulong(dirent->deFileSize);
    uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    
    int clusters_meta = (bytes_needed + cluster_size - 1) / cluster_size;   //number of clusters in metadata
    int clusters_fat = 0;                                                   //number of clusters in FAT   
    
    //go through the cluster chain of FAT and increment the number of clusters in FAT    
    while(is_valid_cluster(cluster,bpb)){
        if(option==0){
            clusters_fat++; 
        }else if(option==1){
            arr[cluster]=0;
        }
        
        //if the next cluster is bad, we change the pointer of the current cluster
        uint16_t next = get_fat_entry(cluster, image_buf, bpb);
        if(get_fat_entry(next, image_buf, bpb) == (CLUST_BAD&FAT12_MASK)){
            printf("\t*BAD:\tBad cluster %d detected and removed from chain.\n",next);
            set_fat_entry(cluster, next+1, image_buf, bpb);
            cluster = next+1;
        }else{
            cluster = next;
        }
        
    }
    if(option==0){        
        check_cluster_number(dirent, image_buf, bpb, clusters_meta, clusters_fat);
    }
          
}

void print_indent(int indent){
    for (int i = 0; i < indent*4; i++)
	    printf(" ");
}

/*
 * Go through the metadata, check if the starting cluster number is larger or equal to 2, and print out useful information about the directory entry
 */
uint16_t scan_dirent(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb, int indent, int option, int arr[]){
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY){
    	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED){
    	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E){
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--){
    	if (name[i] == ' ') 
	        name[i] = '\0';
    	else 
	        break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--){
    	if (extension[i] == ' ') 
	        extension[i] = '\0';
	    else 
	        break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
    }else if ((dirent->deAttributes & ATTR_VOLUME) != 0){
        if(option==0){
        	printf("Volume: %s\n", name);
        }
    
    }else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	    if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
	        if(option==0){
        	    print_indent(indent);
        	    printf("%s/ (directory)\n", name);
        	}
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
        
    }else{
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
    	int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	    int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
    	int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
    	int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

	    size = getulong(dirent->deFileSize);
	    if(option==0){
	        print_indent(indent);
	        printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	               name, extension, size, getushort(dirent->deStartCluster),
	               ro?'r':' ', 
                       hidden?'h':' ', 
                       sys?'s':' ', 
                       arch?'a':' ');
            if(getushort(dirent->deStartCluster)<2){
	            printf("\t*BAD:\tStarting cluster number smaller than 2.\n");
	        }
        }                   
        FAT_scan(dirent, image_buf, bpb, option, arr);        
    }
    return followclust;
}

void follow_dir(uint16_t cluster, int indent, uint8_t *image_buf, struct bpb33* bpb, int option, int arr[]){
    while (is_valid_cluster(cluster, bpb)){
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	    for ( ; i < numDirEntries; i++){            
            uint16_t followclust = scan_dirent(dirent, image_buf, bpb, indent, option, arr); //do not need
            if (followclust){
                follow_dir(followclust, indent+1, image_buf, bpb, option, arr);
            }
            dirent++;
    	}
	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

/*
 * Option 0 is used when we want to compare and modify the number of cluters in metadata and FAT.
 * Option 1 is used when we want to find orphaned clusters.
 */
void traverse_root(uint8_t *image_buf, struct bpb33* bpb, int option, int arr[]){
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++){
        uint16_t followclust = scan_dirent(dirent, image_buf, bpb, 0, option, arr);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, 1, image_buf, bpb, option, arr);
        dirent++;
    }
}


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void write_dirent(struct direntry *dirent, char *filename, uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

}

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    return;
	}
	dirent++;
    }
}

/*
 * Go through metadata and save orphans
 */
void check_unassigned(uint8_t *image_buf, struct bpb33* bpb){
    
    int total_clusters = bpb->bpbSectors / bpb->bpbSecPerClust;
    int clusters_status[total_clusters];
    for(int i=2; i<total_clusters; i++){
        if(get_fat_entry(i, image_buf, bpb) == (CLUST_FREE&FAT12_MASK) || get_fat_entry(i, image_buf, bpb) == (CLUST_BAD&FAT12_MASK)){
		    clusters_status[i]=0;	
	    }else{
	        clusters_status[i]=1;
	    }  
    }
    traverse_root(image_buf,bpb,1,clusters_status);
    
    int clusters_unassigned[total_clusters];
    int num_orphans = 0;
    printf("\n");
    for(int i=5; i<total_clusters; i++){
        if(clusters_status[i]==1){
            clusters_unassigned[num_orphans++] = i;
        }
    }
    for(int j=0; j<num_orphans; j++){
        char name[MAXFILENAME];
        sprintf(name,"FOUND%d.DAT",j+1);
        printf("*BAD:\tCluster %d is unassigned but not freed. Now in directory as %s.\n",clusters_unassigned[j],name);  
        struct direntry *dirent = (struct direntry*)cluster_to_addr(0, image_buf, bpb);
        long size = bpb->bpbSecPerClust * bpb->bpbBytesPerSec; 
        create_dirent(dirent, name, clusters_unassigned[j], size, image_buf, bpb);
    }
    
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if(argc < 2){
    	usage(argv[0]);
    }   
    
    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
                                
    printf("\n");
    int empty[1] = {0};
    traverse_root(image_buf, bpb, 0, empty);

    check_unassigned(image_buf, bpb);
    unmmap_file(image_buf, &fd);
    return 0;
}
