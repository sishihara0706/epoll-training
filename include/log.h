#ifndef EPOLL_TRAINING_LOG_H
#define EPOLL_TRAINING_LOG_H

#define LOG_ERROR 0
#define LOG_INFO  1
#define LOG_DEBUG 2

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_INFO
#endif

#define LOG(level, ...)							\
	do {										\
		if ((level) <= LOG_LEVEL) {				\
            fprintf(stderr, "[%s] ",			\
                (level) == LOG_ERROR ? "ERROR" :\
                (level) == LOG_INFO  ? "INFO"  :\
                                       "DEBUG");\
            fprintf(stderr, __VA_ARGS__);		\
            fputc('\n', stderr);				\
        }										\
    } while (0)
#endif
