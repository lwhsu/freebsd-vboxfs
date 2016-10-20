--- include/VBox/types.h.orig	2016-10-21 03:06:00.170717081 +0800
+++ include/VBox/types.h	2016-10-21 03:08:08.698331689 +0800
@@ -75,6 +75,10 @@
 
 
 /** Pointer to a VM. */
+/** XXX: ditry hack for not conflict with PVM in sys/priority.h */
+#if defined(RT_OS_FREEBSD) && defined(_KERNEL)
+# undef PVM
+#endif
 typedef struct VM                  *PVM;
 /** Pointer to a VM - Ring-0 Ptr. */
 typedef R0PTRTYPE(struct VM *)      PVMR0;
