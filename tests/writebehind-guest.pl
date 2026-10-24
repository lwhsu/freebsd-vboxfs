#!/usr/bin/env perl
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Guest side of tests/writebehind-check.sh: one scenario per invocation,
# each exercising a corner of data written behind (delayed writes).  The
# host side looks at the share while a scenario is in progress and after
# it.  Prints "ok ..." or "BAD ..." lines; exits non-zero on BAD.
#
# usage: writebehind-guest.pl SCENARIO DIR
#
use strict;
use warnings;
use Fcntl qw(:DEFAULT SEEK_SET);
use Time::HiRes qw(sleep time);

my ($scen, $dir) = @ARGV;
die "usage: $0 SCENARIO DIR\n" unless defined $scen && defined $dir;
my $bad = 0;
sub ok  { print "ok $_[0]\n"; }
sub bad { print "BAD $_[0]\n"; $bad = 1; }
sub check { my ($cond, $what) = @_; $cond ? ok($what) : bad($what); }
sub now_ms { return int(time() * 1000); }

if ($scen eq "delayed") {
	# A partial block written and left open: the host must see it within
	# the write delay (vfs.vboxvfs.writedelay, 1 s) plus a syncer tick,
	# and the rest at close(2).
	open(my $fh, '>', "$dir/delayed.txt") or die "open: $!";
	$fh->autoflush(1);
	print $fh "hello\n";
	printf "wrote hello %d\n", now_ms();
	sleep 3;
	print $fh "world\n";
	printf "wrote world %d\n", now_ms();
	close($fh) or bad("close delayed.txt: $!");
	printf "closed %d\n", now_ms();
} elsif ($scen eq "seq4k") {
	# 64 writes of 4 KiB: one host request per 64 KiB block, same bytes.
	sysopen(my $fh, "$dir/seq4k.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644)
	    or die "open: $!";
	for my $i (0 .. 63) {
		my $buf = chr(65 + $i % 26) x 4096;
		syswrite($fh, $buf) == 4096 or die "write: $!";
	}
	close($fh) or bad("close seq4k.bin: $!");
	ok("seq4k written");
} elsif ($scen eq "unlink") {
	# Temporary-file pattern: write, unlink, read back through the
	# descriptor, close.  Nothing may be left on the host.
	sysopen(my $fh, "$dir/tmpf", O_RDWR | O_CREAT | O_TRUNC, 0644)
	    or die "open: $!";
	my $data = "abc\n" x 1000;
	syswrite($fh, $data) == length($data) or die "write: $!";
	unlink("$dir/tmpf") or die "unlink: $!";
	sysseek($fh, 0, SEEK_SET) or die "seek: $!";
	my $buf = "";
	my $n = sysread($fh, $buf, length($data) + 10);
	check(defined $n && $buf eq $data, "read back after unlink ($n bytes)");
	check(close($fh), "close after unlink");
	check(!-e "$dir/tmpf", "tmpf gone");
} elsif ($scen eq "renameover") {
	# A file with data written behind is renamed over: the host keeps the
	# new file's content; the old descriptor still closes.
	sysopen(my $old, "$dir/target", O_RDWR | O_CREAT | O_TRUNC, 0644)
	    or die "open: $!";
	syswrite($old, "old content, written behind\n");
	open(my $new, '>', "$dir/source") or die "open: $!";
	print $new "new content\n";
	close($new) or die "close: $!";
	rename("$dir/source", "$dir/target") or die "rename: $!";
	syswrite($old, "more old content after rename\n");
	check(close($old), "close of the replaced file");
	open(my $rd, '<', "$dir/target") or die "open: $!";
	my $got = do { local $/; <$rd> };
	close($rd);
	check($got eq "new content\n", "target has the new content");
} elsif ($scen eq "truncate") {
	# Shrink and extend with data still behind.
	sysopen(my $fh, "$dir/trunc.bin", O_RDWR | O_CREAT | O_TRUNC, 0644)
	    or die "open: $!";
	my $data = join("", map { chr(32 + $_ % 95) } 0 .. 99999);
	syswrite($fh, $data) == 100000 or die "write: $!";
	truncate($fh, 1000) or die "truncate: $!";
	close($fh) or bad("close trunc.bin: $!");
	check(-s "$dir/trunc.bin" == 1000, "size 1000 after shrink");
	sysopen($fh, "$dir/trunc.bin", O_RDWR) or die "open: $!";
	syswrite($fh, "0123456789") == 10 or die "write: $!";
	truncate($fh, 5000) or die "truncate: $!";
	close($fh) or bad("close trunc.bin: $!");
	open(my $rd, '<', "$dir/trunc.bin") or die "open: $!";
	my $got = do { local $/; <$rd> };
	close($rd);
	check(length($got) == 5000, "size 5000 after extend");
	check(substr($got, 0, 10) eq "0123456789", "head rewritten");
	check(substr($got, 10, 990) eq substr($data, 10, 990), "old tail kept");
	check(substr($got, 1000) eq "\0" x 4000, "extension is zeros");
} elsif ($scen eq "fsync") {
	# fsync(2) pushes data written behind to the host before close(2).
	open(my $fh, '>', "$dir/fsync.txt") or die "open: $!";
	$fh->autoflush(1);
	print $fh "fsynced\n";
	IO::Handle::sync($fh) == 0 or bad("fsync: $!");
	printf "synced %d\n", now_ms();
	sleep 2;
	close($fh) or bad("close fsync.txt: $!");
} elsif ($scen eq "syncall") {
	# sync(2) (VFS_SYNC through the default vfs_stdsync) pushes data written
	# behind to the host before close(2).
	open(my $fh, '>', "$dir/syncall.txt") or die "open: $!";
	$fh->autoflush(1);
	print $fh "syncall\n";
	system("sync") == 0 or bad("sync(1): $?");
	printf "synced %d\n", now_ms();
	sleep 2;
	close($fh) or bad("close syncall.txt: $!");
} elsif ($scen eq "osync") {
	# O_SYNC writes go through right away.
	sysopen(my $fh, "$dir/osync.txt", O_WRONLY | O_CREAT | O_TRUNC | O_SYNC,
	    0644) or die "open: $!";
	syswrite($fh, "osync\n") == 6 or die "write: $!";
	printf "written %d\n", now_ms();
	sleep 2;
	close($fh) or bad("close osync.txt: $!");
} elsif ($scen eq "append") {
	# A log file: a line every 100 ms, never closed while it grows.  The
	# host must not lag more than the write delay plus a tick behind.
	open(my $fh, '>>', "$dir/append.log") or die "open: $!";
	$fh->autoflush(1);
	for my $i (1 .. 40) {
		printf $fh "line %d %d\n", $i, now_ms();
		sleep 0.1;
	}
	printf "last %d\n", now_ms();
	sleep 3;
	close($fh) or bad("close append.log: $!");
} elsif ($scen eq "stat") {
	# stat(2) shows a write at once, before the host has seen it.
	my $t0 = time();
	sysopen(my $fh, "$dir/stat.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644)
	    or die "open: $!";
	syswrite($fh, "x" x 5000) == 5000 or die "write: $!";
	my @st = stat($fh);
	check($st[7] == 5000, "fstat size 5000 while behind ($st[7])");
	check(abs($st[9] - $t0) <= 2, "fstat mtime is now ($st[9] vs $t0)");
	my @st2 = stat("$dir/stat.bin");
	check($st2[7] == 5000, "stat size 5000 while behind ($st2[7])");
	close($fh) or bad("close stat.bin: $!");
	sleep 0.5;
	@st = stat("$dir/stat.bin");
	check($st[7] == 5000, "stat size 5000 after close ($st[7])");
} else {
	die "unknown scenario $scen\n";
}
exit($bad);
