      program test_ckpt_F

      implicit none

      include 'mpif.h'
      include 'scrf.h'

      character*1024 :: basefname = "rank_"
      character*1024 :: fname, file_suffix
      character(len=SCR_MAX_FILENAME) :: fname_scr
      character(len=SCR_MAX_FILENAME) :: ckptname
      integer(kind=4) :: flag;
      integer(kind=4) :: outflags;
      integer(kind=4) :: valid;

      integer, parameter :: ni=20, nj=30, nk=45
      integer :: loop_count=5

      integer(kind=8), dimension(ni,nj,nk) :: W1, R1

      integer :: ierr, errors, all_errors, nprocs, mynod, ios
      integer :: i,j,k,loop

      integer :: writeunit, readunit
      integer(kind=8) :: nodeoff

!      integer (kind = 8) :: total_bytes_transferred
      real (kind = 8) :: total_bytes_transferred
      real (kind = 4) overall_transfer_rate
      real (kind = 8) time0, time1, iter_time0, iter_time1

      call MPI_INIT(ierr)
      call MPI_COMM_SIZE(MPI_COMM_WORLD, nprocs, ierr)
      call MPI_COMM_RANK(MPI_COMM_WORLD, mynod, ierr)

      call SCR_INIT(ierr)

      nodeoff=2**21

!     test restart interface
      call SCR_HAVE_RESTART(flag, ckptname, ierr)
      if (flag == 1) then
        call SCR_START_RESTART(ckptname, ierr)

        readunit = mynod
        write(file_suffix, '(i5.5)') readunit
        fname = trim(ckptname) // "/" // 
     +      trim(basefname) // trim(file_suffix) // ".ckpt"
        call SCR_ROUTE_FILE(fname, fname_scr, ierr)

        R1 = 0
        valid = 1
        open(unit=readunit,file=fname_scr,form='unformatted',
     +      action='read')
        read(readunit,iostat=ios) R1
        close(readunit)

        call SCR_COMPLETE_RESTART(valid, ierr)
      endif

      if (mynod == 0) then
        time0 = MPI_Wtime (ierr)
      endif

!     execute work loop and checkpoint
      do loop = 1, loop_count

        if (mynod == 0) then
          iter_time0 = MPI_Wtime (ierr)
        endif

        forall(i=1:ni,j=1:nj,k=1:nk) W1(i,j,k) = 
     +      nodeoff*mynod+i+ni*(j-1+nj*(k-1))

!       test checkpoint interface
        call SCR_START_CHECKPOINT(ierr)

        write(file_suffix, '(i5.5)') loop
        ckptname = "ckpt_" // trim(file_suffix)

        writeunit = mynod
        write(file_suffix, '(i5.5)') writeunit
        fname = trim(ckptname) // "/" //
     +      trim(basefname) // trim(file_suffix) // ".ckpt"
        call SCR_ROUTE_FILE(fname, fname_scr, ierr)

        valid = 1
        open(unit=writeunit,file=fname_scr,form='unformatted',
     +      action='write')
        write(writeunit,iostat=ios) W1
        close(writeunit)

        call SCR_COMPLETE_CHECKPOINT(valid, ierr)

!       test output interface
        write(file_suffix, '(i5.5)') loop
        ckptname = "output_" // trim(file_suffix)
        outflags = SCR_FLAG_CHECKPOINT + SCR_FLAG_OUTPUT
        call SCR_START_OUTPUT(ckptname, outflags, ierr)

        writeunit = mynod
        write(file_suffix, '(i5.5)') writeunit
        fname = trim(ckptname) // "/" //
     +      trim(basefname) // trim(file_suffix) // ".ckpt"
        call SCR_ROUTE_FILE(fname, fname_scr, ierr)

        valid = 1
        open(unit=writeunit,file=fname_scr,form='unformatted',
     +      action='write')
        write(writeunit,iostat=ios) W1
        close(writeunit)

        call SCR_COMPLETE_OUTPUT(valid, ierr)

        call MPI_BARRIER(MPI_COMM_WORLD, ierr)

        errors = 0
        call MPI_Allreduce(errors, all_errors, 1, MPI_INTEGER,
     +      MPI_SUM,MPI_COMM_WORLD,ierr)
        if( all_errors > 0 ) call exit (1)

        if (mynod == 0) then
          iter_time1 = MPI_Wtime (ierr)
           write (6, 650) loop, iter_time1 - iter_time0
 650       format ("Iteration = ", i6, ", Run Time = ", f10.4, " sec.")
#ifdef __ibmxl__
           call flush_ (6)
#else
           call flush (6)
#endif
        endif

      end do ! loop

      if (mynod .eq. 0) then
        time1 = MPI_Wtime (ierr)
        total_bytes_transferred = real (2 * 6 * loop_count)
        total_bytes_transferred = total_bytes_transferred *
     +      real (ni * nj * nk)
        total_bytes_transferred = total_bytes_transferred *
     +      real (nprocs)

        write (6, 601) total_bytes_transferred / 1.0e+06
 601    format ("Total bytes transferred = ", 1pe12.3, " 10e+06 Bytes")

        write (6, 602) time1 - time0
 602    format ("Total time =              ", 1pe12.3, " sec")

        overall_transfer_rate = real(total_bytes_transferred) /
     +      ((time1 - time0) * 1.0e+06)
        write (6, 600) overall_transfer_rate 
 600    format ("Overall Transfer Rate =   ", 1pe12.3,
     +      " 10e+06 Bytes/sec")
        write (6, 603) 
 603    format ("all done")
      endif

      call SCR_FINALIZE(ierr)

      call MPI_FINALIZE(ierr)

      end program test_ckpt_F
