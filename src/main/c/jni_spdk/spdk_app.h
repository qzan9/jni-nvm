#ifndef _SPDK_APP_H_
#define _SPDK_APP_H_

#ifdef __GNUC__
#	define _SVID_SOURCE
#endif /* __GNUC__ */

#ifdef __cplusplus
extern "C" {
#endif

int spdk_identity(void);

#ifdef __cplusplus
}
#endif

#endif /* _SPDK_APP_H_ */
