/*
 * Count the host requests vboxvfs makes and time them.  Run during a
 * workload: dtrace -s hostreqs.d -c 'sh workload.sh' or for a few seconds
 * with -n 'tick-30s { exit(0); }'.  Needs the module's symbols only (fbt
 * on the exported function names, no CTF).
 */
#pragma D option quiet
fbt::vboxvfs_host_create:entry,
fbt::vboxvfs_host_lookup:entry,
fbt::vboxvfs_host_getinfo:entry,
fbt::vboxvfs_host_setinfo:entry,
fbt::vboxvfs_host_list:entry,
fbt::vboxvfs_host_remove:entry,
fbt::vboxvfs_host_rename:entry,
fbt::vboxvfs_host_close:entry,
fbt::vboxvfs_host_read:entry,
fbt::vboxvfs_host_write:entry,
fbt::vboxvfs_host_flush:entry,
fbt::vboxvfs_host_setsize:entry,
fbt::vboxvfs_host_readlink:entry,
fbt::vboxvfs_host_volinfo:entry
{
	@calls[probefunc] = count();
	self->ts[probefunc] = timestamp;
}
fbt::vboxvfs_host_create:return,
fbt::vboxvfs_host_lookup:return,
fbt::vboxvfs_host_getinfo:return,
fbt::vboxvfs_host_setinfo:return,
fbt::vboxvfs_host_list:return,
fbt::vboxvfs_host_remove:return,
fbt::vboxvfs_host_rename:return,
fbt::vboxvfs_host_close:return,
fbt::vboxvfs_host_read:return,
fbt::vboxvfs_host_write:return,
fbt::vboxvfs_host_flush:return,
fbt::vboxvfs_host_setsize:return,
fbt::vboxvfs_host_readlink:return,
fbt::vboxvfs_host_volinfo:return
/self->ts[probefunc]/
{
	@us[probefunc] = quantize((timestamp - self->ts[probefunc]) / 1000);
	@total[probefunc] = sum((timestamp - self->ts[probefunc]) / 1000);
	self->ts[probefunc] = 0;
}
fbt::vboxvfs_host_read:entry { @readbytes = sum(arg5); }
fbt::vboxvfs_host_write:entry { @writebytes = sum(arg5); }
END
{
	printf("\n== host requests ==\n"); printa("%-24s %@12d\n", @calls);
	printf("\n== total time in requests (us) ==\n"); printa("%-24s %@12d\n", @total);
	printf("\n== bytes ==\n"); printa("read  %@d\n", @readbytes); printa("write %@d\n", @writebytes);
	printf("\n== latency (us) ==\n"); printa(@us);
}
