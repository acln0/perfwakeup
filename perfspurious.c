#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

static int perf_event_open(struct perf_event_attr *a, pid_t pid, int cpu,
			   int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, a, pid, cpu, group_fd, flags);
}

static void evsig(int evfd)
{
	unsigned long long val = 1ULL;
	write(evfd, &val, sizeof(unsigned long long));
}

static void evwait(int evfd)
{
	unsigned long long val;
	read(evfd, &val, sizeof(unsigned long long));
}

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

void parent(int evstart, int evgetpid, int evquit, pid_t pid)
{
	struct perf_event_attr a;

	memset(&a, 0, sizeof(struct perf_event_attr));

	a.type = PERF_TYPE_TRACEPOINT;
	a.config = 235; // sys_enter_getpid
	a.disabled = 1;
	a.sample_period = 1;
	a.sample_type |= PERF_SAMPLE_TID;
	a.wakeup_events = 1;

	int perffd = perf_event_open(&a, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
	if (perffd == -1)
		die("perf_event_open");

	int res = fcntl(perffd, F_SETFL, O_NONBLOCK);
	if (res != 0)
		die("F_SETFL");

	int epollfd = epoll_create1(EPOLL_CLOEXEC);
	if (epollfd == -1)
		die("epoll_create1");

	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = perffd;

	res = epoll_ctl(epollfd, EPOLL_CTL_ADD, perffd, &ev);
	if (res != 0)
		die("epoll_ctl");

	void *ring = mmap(NULL, 129 * getpagesize(), PROT_READ | PROT_WRITE,
			  MAP_SHARED, perffd, 0);
	if (ring == NULL)
		die("mmap");
	struct perf_event_mmap_page *meta = ring;

	ioctl(perffd, PERF_EVENT_IOC_RESET, 0);
	ioctl(perffd, PERF_EVENT_IOC_ENABLE, 0);

	printf("parent: signaling evstart\n");
	evsig(evstart);

	printf("parent: waiting for evgetpid\n");
	evwait(evgetpid);

	printf("parent: reading ring\n");

	unsigned long long head;
	unsigned long long tail;
again:
	head = meta->data_head;
	tail = meta->data_tail;

	asm volatile("" ::: "memory");

	printf("parent: ring head = %d, tail = %d\n", head, tail);

	if (head == tail) {
		printf("parent: polling...\n");
		struct timespec start;
		struct timespec end;
		clock_gettime(CLOCK_MONOTONIC, &start);

		struct epoll_event perfev;
		int n = epoll_wait(epollfd, &perfev, 1, -1);

		clock_gettime(CLOCK_MONOTONIC, &end);
		long diff = end.tv_nsec - start.tv_nsec;
		printf("parent: poll returned in %ldns\n", diff);

		if (perfev.events & EPOLLIN)
			printf("parent: spurious: saw EPOLLIN on perf fd\n");
	} else {
		printf("parent: advancing\n");
		asm volatile("" ::: "memory");
		meta->data_tail = tail + 16;
		goto again;
	}

	printf("parent: signaling evquit\n");
	evsig(evquit);

	int cstatus;
	waitpid(pid, &cstatus, 0);
	printf("parent: child %d exited with code %d\n", pid, cstatus);
}

void child(int evstart, int evgetpid, int evquit)
{
	printf("child: waiting for evstart\n");
	evwait(evstart);

	printf("child: calling getpid\n");
	getpid();

	printf("child: signaling evgetpid\n");
	evsig(evgetpid);

	printf("child: waiting for evquit\n");
	evwait(evquit);

	printf("child: exiting\n");
	exit(0);
}

int main(int argc, char **argv)
{
	int n = 8;
	int evstart = eventfd(0, EFD_SEMAPHORE);
	if (evstart == -1)
		die("eventfd");

	int evgetpid = eventfd(0, EFD_SEMAPHORE);
	if (evgetpid == -1)
		die("eventfd");

	int evquit = eventfd(0, EFD_SEMAPHORE);
	if (evquit == -1)
		die("eventfd");

	printf("parent: forking\n");
	pid_t pid = fork();
	switch (pid) {
	case -1:
		die("fork");
	case 0:
		child(evstart, evgetpid, evquit);
		break;
	default:
		parent(evstart, evgetpid, evquit, pid);
		break;
	}
}
