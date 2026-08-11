#ifndef FAKEFD_H
#define FAKEFD_H

int fakefd_is_fake(int fd);
int fakefd_pipe(int fds[2]);
long fakefd_read(int fd, void *buffer, unsigned long size);
long fakefd_write(int fd, const void *buffer, unsigned long size);
int fakefd_close(int fd);

#endif
