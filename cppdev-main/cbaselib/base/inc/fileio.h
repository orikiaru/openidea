#ifndef file_io_h__j
#define file_io_h__j
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#else
#include <aio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <string>
#include <assert.h>
#include <string.h>

#include "bytes.h"
#include "spin_lock.h"
enum
{
	F_OP_NULL	= 0,
	F_OP_READ 	= O_RDONLY,
	F_OP_WRITE 	= O_WRONLY | O_CREAT, 
	F_OP_RW		= F_OP_READ | F_OP_WRITE,
};

class FILE_IO
{
	int		_fd;
	Bytes		_ibuff;
	Bytes		_obuff;
	int		_op;
	Mutex		_locker;
public:
	FILE_IO(int fd, int op):_fd(fd), _op(op){}
	FILE_IO():_fd(-1),_op(F_OP_NULL){}
	~FILE_IO()	{
#ifdef _WIN32
		if (_fd >= 0) _close(_fd);
#else
		if (_fd >= 0) close(_fd);
#endif
	}
	bool OpenFile(std::string file_name, int op)
	{
		assert(_fd == -1);
		_op = op;
#ifdef _WIN32
		_fd = _open(file_name.c_str(), _op, _S_IREAD | _S_IWRITE);
#else
		_fd = open(file_name.c_str(), _op, 0660);
#endif
		if(_fd == -1)
		{
			return false;
		}
		return true;
	}
	bool SyncReadAll(int offset = 0)
	{
		MutexGuard l(_locker);
		assert(_fd != -1);
#ifdef _WIN32
		_lseek(_fd, 0, SEEK_SET);
#else
		lseek(_fd, 0, SEEK_SET);
#endif
		if(offset > 0)
		{
#ifdef _WIN32
			if(-1 == _lseek(_fd, offset, SEEK_SET))
#else
			if(-1 == lseek(_fd, offset, SEEK_SET))
#endif
			{
				perror("lseek");
				return false;
			}
		}
		Bytes tmp_bytes;	
		tmp_bytes.Reserve(8192);
		for(;;)
		{
#ifdef _WIN32
			int read_num = _read(_fd, tmp_bytes.Begin(), (unsigned)tmp_bytes.Captity());
#else
			int read_num = read(_fd, tmp_bytes.Begin(), tmp_bytes.Captity());
#endif
			if(read_num == -1 && errno != EAGAIN && errno != EINTR)
			{
				perror("SyncReadAll");	
				goto read_fault;
			}
			if(read_num == 0)
			{
				goto read_finish;
			}
			if(read_num > 0)
			{
				_ibuff.Append(tmp_bytes.Begin(), read_num);
			}
		}
	read_finish:
		return true;
	read_fault:
		return false;
	}

	void AppendData(Bytes& obuff)
	{
		_obuff.Append(obuff);
	}

	bool SyncWriteAll(Bytes& obuff, int offset = 0)
	{
		_obuff.Append(obuff);	
		MutexGuard l(_locker);
		assert(_fd != -1);
#ifdef _WIN32
		_lseek(_fd, 0, SEEK_SET);
#else
		lseek(_fd, 0, SEEK_SET);
#endif
		if(offset > 0)
		{
#ifdef _WIN32
			if(-1 == _lseek(_fd, offset, SEEK_SET))
#else
			if(-1 == lseek(_fd, offset, SEEK_SET))
#endif
			{
				perror("lseek");
				return false;
			}
		}

		for(;;)
		{
#ifdef _WIN32
			int write_num = _write(_fd, _obuff.Begin(), (unsigned)_obuff.Size());
#else
			int write_num = write(_fd, _obuff.Begin(), _obuff.Size());
#endif
			if(write_num == 0)
			{
				goto write_finish;
			}
			else if(write_num == -1 && errno != EAGAIN && errno != EINTR)
			{
				perror("Net_IO_Session::DataOut");	
				goto write_fault;
			}
			if(write_num > 0)
			{
				_obuff.Erase(0, write_num);
			}
		}
write_finish:
		return true;
write_fault:
		return false;
	}

	Bytes& GetIBuff()	{	return _ibuff;	}
	Bytes& GetOBuff()	{	return _obuff;	}
	//struct aiocb {
	/* The order of these fields is implementation-dependent */

//	int             aio_fildes;     /* File descriptor */
//	off_t           aio_offset;     /* File offset */
//	volatile void  *aio_buf;        /* Location of buffer */
//	size_t          aio_nbytes;     /* Length of transfer */
//	int             aio_reqprio;    /* Request priority */
//	struct sigevent aio_sigevent;   /* Notification method */
//	int             aio_lio_opcode; /* Operation to be performed;
//					   lio_listio() only */
	/* Various implementation-internal fields not shown */
//};
/* Operation codes for 'aio_lio_opcode': */
//enum { LIO_READ, LIO_WRITE, LIO_NOP };
//    aio_read(3)     Enqueue a read request.  This is the asynchronous analog of read(2).
//      aio_write(3)    Enqueue a write request.  This is the asynchronous analog of write(2).
//      aio_fsync(3)    Enqueue a sync request for the I/O operations on a file descriptor.  This is the asynchronous analog of fsync(2) and fdatasync(2).
//      aio_error(3)    Obtain the error status of an enqueued I/O request.
//      aio_return(3)   Obtain the return status of a completed I/O request.
//      aio_suspend(3)  Suspend the caller until one or more of a specified set of I/O requests completes.
//      aio_cancel(3)   Attempt to cancel outstanding I/O requests on a specified file descriptor.
//      lio_listio(3)   Enqueue multiple I/O requests using a single function call.


	bool ASyncReadAll(int offset = 0)
	{
#ifdef _WIN32
		(void)offset;
		return false;
#else
		MutexGuard l(_locker);
		assert(_fd != -1);
		struct stat st;
		fstat(_fd, &st);
		_ibuff.Reserve(st.st_size);
		aiocb* pcb = (aiocb*)malloc(sizeof(aiocb));
		memset(pcb, 0, sizeof(aiocb));
		pcb->aio_fildes = _fd;
		pcb->aio_offset = offset;
		pcb->aio_buf = _ibuff.Begin();
		pcb->aio_nbytes = _ibuff.Captity();
		pcb->aio_reqprio = 0;
		pcb->aio_lio_opcode = LIO_READ;
		return true;
#endif
	}
};












#endif
