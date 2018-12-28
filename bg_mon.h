#ifndef _BG_MON_H_
#define _BG_MON_H_

#ifdef DEBUG
#define DEBUG_TEST 0
#else
#define DEBUG_TEST 1
#endif

#define debug_print(fmt, ...) 													\
		do { if (DEBUG_TEST)  													\
			ereport(DEBUG5, (errmsg("%s:%d:%s(): " fmt, 						\
							__FILE__, __LINE__, __func__, ##__VA_ARGS__))); } 	\
		while (0)

#endif /* _BG_MON_H_ */
