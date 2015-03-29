/* BEGIN A3 SETUP */
/* This file existed for A1 and A2, but has been completely replaced for A3.
 * We have kept the dumb versions of sys_read and sys_write to support early
 * testing, but they should be replaced with proper implementations that 
 * use your open file table to find the correct vnode given a file descriptor
 * number.  All the "dumb console I/O" code should be deleted.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <vfs.h>
#include <vnode.h>
#include <uio.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <copyinout.h>
#include <synch.h>
#include <file.h>
#include <kern/seek.h> // seek type in sys_lseek()

/* This special-case global variable for the console vnode should be deleted 
 * when you have a proper open file table implementation.
 */
struct vnode *cons_vnode=NULL; 

/* This function should be deleted, including the call in main.c, when you
 * have proper initialization of the first 3 file descriptors in your 
 * open file table implementation.
 * You may find it useful as an example of how to get a vnode for the 
 * console device.
 */
void dumb_consoleIO_bootstrap()
{
	int result;
	char path[5];

	/* The path passed to vfs_open must be mutable.
	* vfs_open may modify it.
	*/

	strcpy(path, "con:");
	result = vfs_open(path, O_RDWR, 0, &cons_vnode);

	if (result) {
	/* Tough one... if there's no console, there's not
	 * much point printing a warning...
	 * but maybe the bootstrap was just called in the wrong place
	 */
	kprintf("Warning: could not initialize console vnode\n");
	kprintf("User programs will not be able to read/write\n");
	cons_vnode = NULL;
	}
}

/*
 * mk_useruio
 * sets up the uio for a USERSPACE transfer. 
 */
static
void
mk_useruio(struct iovec *iov, struct uio *u, userptr_t buf, 
	   size_t len, off_t offset, enum uio_rw rw)
{

	iov->iov_ubase = buf;
	iov->iov_len = len;
	u->uio_iov = iov;
	u->uio_iovcnt = 1;
	u->uio_offset = offset;
	u->uio_resid = len;
	u->uio_segflg = UIO_USERSPACE;
	u->uio_rw = rw;
	u->uio_space = curthread->t_addrspace;
}

/*
 * sys_open
 * just copies in the filename, then passes work to file_open.
 * You have to write file_open.
 * 
 */
int
sys_open(userptr_t filename, int flags, int mode, int *retval)
{
	char *fname;
	int result;

	if ( (fname = (char *)kmalloc(__PATH_MAX)) == NULL) {
		*retval = -1; // need this??
		return ENOMEM;
	}

	result = copyinstr(filename, fname, __PATH_MAX, NULL);
	if (result) {
		*retval = -1; // need this??
		kfree(fname);
		return result;
	}

	result =  file_open(fname, flags, mode, retval);
	kfree(fname);
	return result;
}

/* 
 * sys_close
 * You have to write file_close.
 */
int
sys_close(int fd)
{
	return file_close(fd);
}

/* 
 * sys_dup2
 * 
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{
	struct file_handle *of;
	//invalid fds
	if (oldfd < 0 || newfd < 0 || newfd >= __OPEN_MAX || oldfd >= __OPEN_MAX){
		return EBADF;
	}

	of = curthread->t_filetable->file_handles[oldfd];
	if (of == NULL) {
		// no open file in the file table at oldfd
		return EBADF; 
	}

	// if there is an opened file at newfd, close it
	if (curthread->t_filetable->file_handles[newfd] != NULL){
		file_close(newfd);
	}

	curthread->t_filetable->file_handles[newfd] = of;

	lock_acquire(of->flock);
	of->ref_count++;
	lock_release(of->flock);

	*retval = newfd;
	return 0;
}

/*
 * sys_read
 * calls VOP_READ.
 * 
 * A3: This is the "dumb" implementation of sys_read:
 * it only deals with file descriptors 1 and 2, and 
 * assumes they are permanently associated with the 
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
	struct uio user_uio;
	struct iovec user_iov;
	int result;
	int offset = 0;
	struct file_handle *of;


	if (fd < 0 || fd >= __OPEN_MAX){
		return EBADF;
	}

	// get the file handle the fd refers to
	of = curthread->t_filetable->file_handles[fd];
	if (of == NULL){
		// error if no file opened at fd in the file table
		return EBADF;
	}

	// check if the vnode associated with the opened file is valid
	if (of->fvnode == NULL){
		// error if no such device
		return ENODEV;
	}

	lock_acquire(of->flock);

	// get the current position(offset) in the opened file
	offset = of->cur_po; 
	/* set up a uio with the buffer, its size, and the current offset */
	mk_useruio(&user_iov, &user_uio, buf, size, offset, UIO_READ);

	/* does the read */
	result = VOP_READ(of->fvnode, &user_uio);
	if (result) {
		//release the lock
		lock_release(of->flock);
		return result;
	}

	// advance the current offset
	// ??????? should we check if the advanced offset exceeds the file size???????
	of->cur_po += size;

	/*
	 * The amount read is the size of the buffer originally, minus
	 * how much is left in it.
	 */
	*retval = size - user_uio.uio_resid;

	//release the lock
	lock_release(of->flock);

	return 0;
}

/*
 * sys_write
 * calls VOP_WRITE.
 *
 * A3: This is the "dumb" implementation of sys_write:
 * it only deals with file descriptors 1 and 2, and 
 * assumes they are permanently associated with the 
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */

int
sys_write(int fd, userptr_t buf, size_t len, int *retval) 
{
    struct uio user_uio;
    struct iovec user_iov;
    int result;
    int offset = 0;
	struct file_handle *of;


	if (fd < 0 || fd >= __OPEN_MAX){
		return EBADF;
	}

	// get the file handle the fd refers to
	of = curthread->t_filetable->file_handles[fd];
	if (of == NULL){
		// error if no file opened at fd in the file table
		return EBADF;
	}

	// check if the vnode associated with the opened file is valid
	if (of->fvnode == NULL){
		// error if no such device
		return ENODEV;
	}

	lock_acquire(of->flock);
	// get the current position(offset) in the opened file
	offset = of->cur_po; 		
    /* set up a uio with the buffer, its size, and the current offset */
    mk_useruio(&user_iov, &user_uio, buf, len, offset, UIO_WRITE);

    /* does the write */
    result = VOP_WRITE(of->fvnode, &user_uio);
    if (result) {
		//release the lock
		lock_release(of->flock);
		return result;
    }
    //advance current position by the number of bytes written
    of->cur_po += len;
    /*
     * the amount written is the size of the buffer originally,
     * minus how much is left in it.
     */
    *retval = len - user_uio.uio_resid;

    return 0;
}

/*
 * sys_lseek
 * 
 */
int
sys_lseek(int fd, off_t offset, int whence, off_t *retval)
{
	struct stat *vn_stat;
	int result;
	off_t new_pos;
	struct file_handle *of;
	// seek on invalid fds will fail
	if (fd < 0 || fd >= __OPEN_MAX){
		return EBADF;
	}
	//seek on console device is not supported
	if (fd >=0 && fd < 3){
		return ESPIPE;
	}

	// get the file handle the fd refers to
	of = curthread->t_filetable->file_handles[fd];
	if (of == NULL){
		// error if no file opened at fd in the file table
		return EBADF;
	}

	lock_acquire(of->flock);
	switch (whence) {
		case SEEK_SET:
			new_pos = offset;
			break;
		case SEEK_CUR:
			new_pos = of->cur_po + offset;
			break;
		case SEEK_END:
			result = VOP_STAT(of->fvnode, vn_stat);
			if (result){
				lock_release(of->flock);
				return result;
			}
			new_pos = vn_stat->st_size + offset;
			break;
		default:
			//whence mode is invalid
			lock_release(of->flock);
			return EINVAL;
	}

	// invalid if resulting seek position is negative
	if (new_pos < 0){
		return EINVAL;
	}

	//vop_tryseek ? to check if the new seek position is legal

	// set the new seek position to the vnode
	of->cur_po = new_pos;
	
	lock_release(of->flock);

	*retval = new_pos;

	return 0;
}


/* really not "file" calls, per se, but might as well put it here */

/*
 * sys_chdir
 * 
 */
int
sys_chdir(userptr_t path)
{
        (void)path;

	return EUNIMP;
}

/*
 * sys___getcwd
 * 
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{
        (void)buf;
        (void)buflen;
        (void)retval;

	return EUNIMP;
}

/*
 * sys_fstat
 */
int
sys_fstat(int fd, userptr_t statptr)
{
        (void)fd;
        (void)statptr;

	return EUNIMP;
}

/*
 * sys_getdirentry
 */
int
sys_getdirentry(int fd, userptr_t buf, size_t buflen, int *retval)
{
        (void)fd;
        (void)buf;
	(void)buflen;
        (void)retval;

	return EUNIMP;
}

/* END A3 SETUP */




