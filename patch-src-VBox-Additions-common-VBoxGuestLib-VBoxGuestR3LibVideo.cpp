--- src/VBox/Additions/common/VBoxGuestLib/VBoxGuestR3LibVideo.cpp.orig	2016-02-08 02:30:14.459179000 +0800
+++ src/VBox/Additions/common/VBoxGuestLib/VBoxGuestR3LibVideo.cpp	2016-02-08 02:25:31.782136000 +0800
@@ -316,7 +316,7 @@
     uint32_t u32ClientId = 0;
     const char *pszPattern = VIDEO_PROP_PREFIX"*";
     PVBGLR3GUESTPROPENUM pHandle = NULL;
-    const char *pszName;
+    const char *pszName = NULL;
     unsigned cHighestScreen = 0;
 
     AssertPtrReturn(pcScreen, VERR_INVALID_POINTER);
