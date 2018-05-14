#include <linux/linkage.h>
#include <linux/moduleloader.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm-generic/errno-base.h>
#include <linux/path.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/cred.h>
#include "custom.h"				// custom header file containing common definitions

#define BUFF_SIZE 512			// Buffer size for larger allocations
#define MIN_BUFF 5				// Min buffer size for allocations
#define DBG_PRINTK printk(KERN_DEBUG "%s::%s::%d\n", __FILE__,__func__,__LINE__)

asmlinkage extern int (*sysptr)(void *args);									// prototype for main system call function
static int check_file_permissions(struct file *, struct file *, int );			// function for file permission check
static int do_full_dedup(struct file *, struct file *, int );					// function to perform full deduplication
static int get_common_bytes(struct file *, struct file *, int );				// function that calculates common bytes between files
static char *allocate_memory(int);												// function for allocating memory
static int check_file_size_and_owners(struct path *, struct path *);						// function to compare file size
static int do_partial_dedup(struct file *, struct file *, struct file *, int ); // function to perform partial deduplication

asmlinkage int xdedup(void *args)
{	
	struct arguments *argmnts = (struct arguments *) args;
	struct file *filp1 = NULL;
	struct file *filp2 = NULL;
	struct file *outfile = NULL;
	mm_segment_t oldfs;
	int ret_val = 0;
	int dbg_flag = 0;	

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	
	filp1 = filp_open(argmnts->inputfile1, O_RDONLY, 0);

	if (!filp1 || IS_ERR(filp1)){
		printk("Unable to open first input file, err: %d", (int) PTR_ERR(filp1));
		return -EINVAL;
	}
	
	filp2 = filp_open(argmnts->inputfile2,O_RDONLY,0);
	
	if (!filp2 || IS_ERR(filp2)){
		filp_close(filp1,NULL);
		printk("Unable to open second input file, err: %d", (int) PTR_ERR(filp2));
		return -EINVAL;
	}

	if (check_file_permissions(filp1, filp2, 1) != 0)
		return -EPERM;

	switch(argmnts->flag){
		case NO_DEDUP|DEBUG|PARTIAL_DEDUP:
		case NO_DEDUP|DEBUG:
			dbg_flag = 1;
		case NO_DEDUP|PARTIAL_DEDUP:
		case NO_DEDUP:
			ret_val = get_common_bytes(filp1, filp2, dbg_flag);
			break;

		case PARTIAL_DEDUP|DEBUG:
			dbg_flag = 1;
		case PARTIAL_DEDUP:
			if(strlen(argmnts->outputfile) > 0){
				outfile = filp_open(argmnts->outputfile, O_WRONLY|O_CREAT, 0644);
				if(!outfile || IS_ERR(outfile)){
					printk("Unable to open outputfile err:%d\n", (int) PTR_ERR(outfile));
					ret_val = -EPERM;
					goto exit1;
				}
				outfile->f_pos = 0;
				ret_val = do_partial_dedup(filp1, filp2, outfile, dbg_flag);
				filp_close(outfile, NULL);
				goto exit1;
			} else
				ret_val = -EINVAL;
			break;

		case DEBUG:
		default:
			if ((ret_val = check_file_size_and_owners(&filp1->f_path, &filp2->f_path)) == 0)
				ret_val = do_full_dedup(filp1, filp2, dbg_flag);
			break;
	}

	if (ret_val < 0)
		printk("Error encountered during dedup %d", ret_val);
	
	exit1:
		filp_close(filp1, NULL);
		filp_close(filp2, NULL);

	set_fs(oldfs);
	return ret_val;
}

static int do_full_dedup(struct file *fp1, struct file *fp2, int dbg)
{
	struct file *temp_file = NULL;
	struct path *path1 = NULL;
	struct path *path2 = NULL;
	struct path *temp_path = NULL;
	struct inode *pinode1 = NULL;
	struct inode *pinode2 = NULL;
	struct inode *pinode_temp = NULL;
	struct dentry *trap = NULL;
	int common_bytes = 0;	
	int retval = 0;
	
	if (dbg)
		printk("%s::%s::%d::Fetching number of common bytes between files\n", __FILE__, __func__, __LINE__);

	common_bytes = get_common_bytes(fp1, fp2, dbg);
	
	path1 = &fp1->f_path;
	path2 = &fp2->f_path;	

	if (common_bytes < 0){
		printk("Error during file comparison\n");
		return common_bytes;
	} else if(common_bytes != path1->dentry->d_inode->i_size){
		printk("Common bytes did not equal file size\n");
		return -EIO;
	} else {
		if (dbg)
			printk("%s::%s::%d::Starting the deduplication process\n", __FILE__, __func__, __LINE__);

		pinode2 = path2->dentry->d_parent->d_inode;	
	
		if (pinode2 == NULL)
			return -ENOENT;

		temp_file = filp_open("xdedup_temp.bak.txt", O_RDWR|O_CREAT, 0644);
		if(!temp_file || IS_ERR(temp_file)){
			printk("Unable to open temp file\n err: %d", (int) PTR_ERR(temp_file));
			return -EPERM;
		}

		temp_path = &temp_file->f_path;
		pinode_temp = temp_path->dentry->d_parent->d_inode;
		
		if (dbg)
			printk("%s::%s::%d::Successfully opened temp file, starting the rename process\n", __FILE__, __func__, __LINE__);

		trap = lock_rename(path2->dentry->d_parent, temp_path->dentry->d_parent);
		
		if (trap != NULL){
			printk("Directories are hierarchial\n");
			unlock_rename(path2->dentry->d_parent, temp_path->dentry->d_parent);
			return -EINVAL;
		}
			
		if (vfs_rename(pinode2, path2->dentry, pinode_temp, temp_path->dentry, NULL, RENAME_EXCHANGE) < 0){
			unlock_rename(path2->dentry->d_parent, temp_path->dentry->d_parent);
			return -EIO;
		}
		unlock_rename(path2->dentry->d_parent, temp_path->dentry->d_parent);
		
		if (dbg)
			printk("%s::%s::%d::Successfully renamed input file 2 to temp file, starting deduplication\n", __FILE__, __func__, __LINE__);

		// Starting actual unlink and link
		pinode1 = path1->dentry->d_parent->d_inode;
		
		if (pinode1 == NULL){
			retval = -ENOENT;
			goto cleanup_rename;
		}
		// Unlink the file2
		inode_lock(pinode_temp);

		retval = vfs_unlink(pinode_temp, temp_path->dentry, NULL);

		inode_unlock(pinode_temp);		

		// If unlink failed go to cleanup
		if(retval < 0)
			goto cleanup_rename;

		if (dbg)
			printk("%s::%s::%d::Successfully unlinked file, now hardlinking to file 1\n", __FILE__, __func__, __LINE__);

		// Link file2 to file1
		retval = vfs_link(path1->dentry, pinode_temp, temp_path->dentry, NULL);

		// If linking failed go to cleanup
		if (retval < 0)
			goto cleanup_unlink;

		retval = common_bytes;

		if (dbg)
			printk("%s::%s::%d::Successfully performed deduplication, exiting\n", __FILE__, __func__, __LINE__);

		goto cleanexit;
	}


	cleanup_rename:
	printk("Error in unlinking file 2, performing cleanup\n");
	lock_rename(temp_path->dentry->d_parent, path2->dentry->d_parent);
	vfs_rename(pinode_temp, temp_path->dentry, pinode2, path2->dentry, NULL, RENAME_EXCHANGE);
	unlock_rename(temp_path->dentry->d_parent, path2->dentry->d_parent);
	goto cleanexit;

	cleanup_unlink:
	printk("Error encountered during file linking, performing cleanup\n");
	vfs_link(path2->dentry, pinode_temp, temp_path->dentry, NULL);

	cleanexit:
	vfs_unlink(pinode2, path2->dentry, NULL);
	filp_close(temp_file, NULL);
	return retval;

}

static int do_partial_dedup(struct file *fp1, struct file *fp2, struct file *op, int dbg)
{
	struct file *temp_file = NULL;
	struct path *temp_path, *output_path;
	struct dentry *trap;
	struct inode *opinode, *temp_inode;
	int common_bytes = 0;
	char *buf1 = NULL;
	int bytes1;
	int min_bytes = 0;
	int retval = 0;

	common_bytes = get_common_bytes(fp1, fp2, dbg);
	retval = common_bytes;
	
	if (dbg)
		printk("%s::%s::%d::Creating temporary file for writing\n", __FILE__, __func__, __LINE__);

	temp_file = filp_open("xdedup_partial_temp.bak.txt", O_RDWR|O_CREAT, 0644);

	if (!temp_file || IS_ERR(temp_file)){
		printk("Unable to open temp file for writing, err: %d", (int) PTR_ERR(temp_file));
		return -EIO;
	}
	
	temp_file->f_pos = 0;	

	if (dbg)
		printk("%s::%s::%d::Reading the file and writing to temporary file\n", __FILE__, __func__, __LINE__);

	buf1 = allocate_memory(common_bytes+1);

	if (buf1 == NULL){
		buf1 = allocate_memory(BUFF_SIZE);
		if (buf1 == NULL){
			retval = -ENOMEM;
			goto cleanexit;
		}
	}

	fp1->f_pos = 0;

	while ( (common_bytes > 0) && (bytes1 = vfs_read(fp1, buf1, ksize(buf1), &fp1->f_pos)) > 0 ){

		min_bytes = (common_bytes < bytes1)?common_bytes:bytes1;

		if(vfs_write(temp_file, buf1, min_bytes, &temp_file->f_pos) < 0){
			printk("Write operation failed\n");
			retval = -EIO;
			goto cleanexit;	
		}
		common_bytes -= min_bytes;
	}
	
	if (common_bytes != 0){
		retval = -EIO;
		goto cleanexit;
	}
	
	if (dbg)
		printk("%s::%s::%d::Copied contents to temporary file, now hardlinking outputfile to temporary file\n", __FILE__, __func__, __LINE__);

	temp_path = &temp_file->f_path;
	output_path = &op->f_path;
	opinode = output_path->dentry->d_parent->d_inode;
	temp_inode = temp_path->dentry->d_parent->d_inode;
	
	trap = lock_rename(temp_path->dentry->d_parent, output_path->dentry->d_parent);
	
	if (trap != NULL){
		printk("File directories are hierarchical\n");
		unlock_rename(temp_path->dentry->d_parent, output_path->dentry->d_parent);
		vfs_unlink(temp_inode, temp_path->dentry, NULL);
		retval = -EINVAL;
		goto cleanexit;
	}
	
	if (vfs_rename(opinode, output_path->dentry, temp_inode, temp_path->dentry, NULL, RENAME_EXCHANGE) < 0){
		unlock_rename(temp_path->dentry->d_parent, output_path->dentry->d_parent);
		vfs_unlink(temp_inode, temp_path->dentry, NULL);
		retval = -EIO;
		goto cleanexit;
	}
	
	
	unlock_rename(temp_path->dentry->d_parent, output_path->dentry->d_parent);
	vfs_unlink(opinode, output_path->dentry, NULL);

	if (dbg)
		printk("%s::%s::%d::Successfully created outputfile with common contents\n", __FILE__, __func__, __LINE__);
	
	cleanexit:
		printk("Performing cleanup before exiting");
		kfree(buf1);
		filp_close(temp_file, NULL);
		
	return retval;
}




static int check_file_permissions(struct file *fp1, struct file *fp2, int dbg)
{
	struct path *path1;
	struct path *path2;
	unsigned long i_ino1;
	unsigned long i_ino2;
	umode_t acc_perm1;
	umode_t acc_perm2;
	u8 *uuid1;
	u8 *uuid2;
	
	if (dbg)
		printk("%s::%s::%d::Performing file permission checks\n", __FILE__, __func__, __LINE__);
	
	// Get Path structure from struct file
	path1 = &fp1->f_path;
	path2 = &fp2->f_path;
	
	// Get file access permissions
	acc_perm1 = path1->dentry->d_inode->i_mode;
	acc_perm2 = path2->dentry->d_inode->i_mode;

	if (dbg)
		printk("%s::%s::%d::Checking file super block UUIDs\n", __FILE__, __func__, __LINE__);

	// Check super block UUID of files
	uuid1 = path1->dentry->d_sb->s_uuid;
	uuid2 = path2->dentry->d_sb->s_uuid;

	if (memcmp(uuid1, uuid2,16) != 0)
		return -EINVAL;	

	if (dbg)
		printk("%s::%s::%d::Ensuring input files are not directories\n", __FILE__, __func__, __LINE__);

	// Check if file is directory
	if( S_ISDIR(acc_perm1) || S_ISDIR(acc_perm2) )
		return -EINVAL;	

	if (dbg)
		printk("%s::%s::%d::Ensuring files are owned by same user\n", __FILE__, __func__, __LINE__);

	if (dbg)
		printk("%s::%s::%d::Ensuring both files are not the same\n", __FILE__, __func__, __LINE__);

	i_ino1 = path1->dentry->d_inode->i_ino;
	i_ino2 = path2->dentry->d_inode->i_ino;

	if (i_ino1 == i_ino2)
		return -EINVAL;

	return 0;
}

static int check_file_size_and_owners(struct path *p1, struct path *p2){

	kuid_t uid_struct = current_uid();
	uid_t process_uid = uid_struct.val;

	if (p1->dentry->d_inode->i_uid.val != p2->dentry->d_inode->i_uid.val)
		return -EPERM;

	if (p1->dentry->d_inode->i_uid.val != process_uid)
		return -EPERM;
	
	if (p1->dentry->d_inode->i_size != p2->dentry->d_inode->i_size)
		return -EINVAL;
	else
		return 0;
}

static int get_common_bytes(struct file *fp1, struct file *fp2, int dbg)
{
	char *buf1 = NULL;
	char *buf2 = NULL;
	int bytes1;
	int bytes2;
	int common_bytes = 0;
	int i;
	
	if (dbg)
		printk("%s::%s::%d::Getting the count of the number of common bytes between files\n", __FILE__, __func__, __LINE__);

	buf1 = allocate_memory(BUFF_SIZE);

	if (buf1 == NULL)
		return -ENOMEM;

	buf2 = allocate_memory(BUFF_SIZE);

	if (buf2 == NULL){
		kfree(buf1);
		return -ENOMEM;
	}
	
	while( (bytes1 = vfs_read(fp1, buf1, ksize(buf1)-1, &fp1->f_pos) ) > 0 && (bytes2 = vfs_read(fp2, buf2, ksize(buf2)-1, &fp2->f_pos) ) > 0 ){
		i = 0;
		while(i < bytes1){
			if (buf1[i] != buf2[i])
				return common_bytes;
			common_bytes += 1;
			i++;
		}
	}
	
	return common_bytes;

}


static char *allocate_memory(int size)
{
	char *buffer = NULL;
	
	buffer = (char *)kmalloc(size, GFP_KERNEL|GFP_NOWAIT);
	
	if (buffer == NULL){
		printk("Failed to allocate memory\n");
		return NULL;
	}

	return (char *)buffer;
}


static int __init init_sys_xdedup(void)
{
	printk("installed new sys_xdedup module\n");
	if (sysptr == NULL)
		sysptr = xdedup;
	return 0;
}
static void  __exit exit_sys_xdedup(void)
{
	if (sysptr != NULL)
		sysptr = NULL;
	printk("removed sys_xdedup module\n");
}
module_init(init_sys_xdedup);
module_exit(exit_sys_xdedup);
MODULE_LICENSE("GPL");
