--- src/VBox/Runtime/r0drv/freebsd/memobj-r0drv-freebsd.c.orig	2015-11-10 21:23:50.000000000 +0000
+++ src/VBox/Runtime/r0drv/freebsd/memobj-r0drv-freebsd.c	2016-01-06 15:31:55.190214000 +0000
@@ -121,16 +121,15 @@
 
         case RTR0MEMOBJTYPE_LOCK:
         {
-            vm_map_t pMap = kernel_map;
-
-            if (pMemFreeBSD->Core.u.Lock.R0Process != NIL_RTR0PROCESS)
-                pMap = &((struct proc *)pMemFreeBSD->Core.u.Lock.R0Process)->p_vmspace->vm_map;
+            struct proc *proc = (struct proc *)pMemFreeBSD->Core.u.Lock.R0Process;
 
-            rc = vm_map_unwire(pMap,
-                               (vm_offset_t)pMemFreeBSD->Core.pv,
-                               (vm_offset_t)pMemFreeBSD->Core.pv + pMemFreeBSD->Core.cb,
-                               VM_MAP_WIRE_SYSTEM | VM_MAP_WIRE_NOHOLES);
-            AssertMsg(rc == KERN_SUCCESS, ("%#x", rc));
+            if (proc != NIL_RTR0PROCESS) {
+                rc = vm_map_unwire(&proc->p_vmspace->vm_map,
+                                   (vm_offset_t)pMemFreeBSD->Core.pv,
+                                   (vm_offset_t)pMemFreeBSD->Core.pv + pMemFreeBSD->Core.cb,
+                                   VM_MAP_WIRE_USER | VM_MAP_WIRE_NOHOLES);
+                AssertMsg(rc == KERN_SUCCESS, ("%#x", rc));
+            }
             break;
         }
 
@@ -168,14 +167,19 @@
             VM_OBJECT_LOCK(pMemFreeBSD->pObject);
 #endif
             vm_page_t pPage = vm_page_find_least(pMemFreeBSD->pObject, 0);
+#if __FreeBSD_version < 900000
+            /* See http://lists.freebsd.org/pipermail/freebsd-current/2012-November/037963.html */
             vm_page_lock_queues();
+#endif
             for (vm_page_t pPage = vm_page_find_least(pMemFreeBSD->pObject, 0);
                  pPage != NULL;
                  pPage = vm_page_next(pPage))
             {
                 vm_page_unwire(pPage, 0);
             }
+#if __FreeBSD_version < 900000
             vm_page_unlock_queues();
+#endif
 #if __FreeBSD_version >= 1000030
             VM_OBJECT_WUNLOCK(pMemFreeBSD->pObject);
 #else
@@ -201,12 +205,12 @@
     vm_page_t pPages;
     int cTries = 0;
 
-#if __FreeBSD_version > 1000000
+#if __FreeBSD_version >= 902508
     int fFlags = VM_ALLOC_INTERRUPT | VM_ALLOC_NOBUSY;
     if (fWire)
         fFlags |= VM_ALLOC_WIRED;
 
-    while (cTries <= 1)
+    while (1)
     {
 #if __FreeBSD_version >= 1000030
         VM_OBJECT_WLOCK(pObject);
@@ -220,18 +224,23 @@
 #else
         VM_OBJECT_UNLOCK(pObject);
 #endif
-        if (pPages)
+        if (pPages || cTries >= 1)
+            break;
+#if __FreeBSD_version >= 1100092
+        if (!vm_page_reclaim_contig(fFlags, cPages, 0, VmPhysAddrHigh, uAlignment, 0))
             break;
+#elif __FreeBSD_version >= 1000015
         vm_pageout_grow_cache(cTries, 0, VmPhysAddrHigh);
+#else
+        vm_contig_grow_cache(cTries, 0, VmPhysAddrHigh);
+#endif
         cTries++;
     }
-
-    return pPages;
 #else
-    while (cTries <= 1)
+    while (1)
     {
         pPages = vm_phys_alloc_contig(cPages, 0, VmPhysAddrHigh, uAlignment, 0);
-        if (pPages)
+        if (pPages || cTries >= 1)
             break;
         vm_contig_grow_cache(cTries, 0, VmPhysAddrHigh);
         cTries++;
@@ -239,11 +248,8 @@
 
     if (!pPages)
         return pPages;
-#if __FreeBSD_version >= 1000030
-    VM_OBJECT_WLOCK(pObject);
-#else
+
     VM_OBJECT_LOCK(pObject);
-#endif
     for (vm_pindex_t iPage = 0; iPage < cPages; iPage++)
     {
         vm_page_t pPage = pPages + iPage;
@@ -255,13 +261,9 @@
             atomic_add_int(&cnt.v_wire_count, 1);
         }
     }
-#if __FreeBSD_version >= 1000030
-    VM_OBJECT_WUNLOCK(pObject);
-#else
     VM_OBJECT_UNLOCK(pObject);
 #endif
     return pPages;
-#endif
 }
 
 static int rtR0MemObjFreeBSDPhysAllocHelper(vm_object_t pObject, u_long cPages,
@@ -291,11 +293,15 @@
             while (iPage-- > 0)
             {
                 pPage = vm_page_lookup(pObject, iPage);
+#if __FreeBSD_version < 900000
                 vm_page_lock_queues();
+#endif
                 if (fWire)
                     vm_page_unwire(pPage, 0);
                 vm_page_free(pPage);
+#if __FreeBSD_version < 900000
                 vm_page_unlock_queues();
+#endif
             }
 #if __FreeBSD_version >= 1000030
             VM_OBJECT_WUNLOCK(pObject);
@@ -515,10 +521,14 @@
      * We could've used vslock here, but we don't wish to be subject to
      * resource usage restrictions, so we'll call vm_map_wire directly.
      */
-    rc = vm_map_wire(pVmMap,                                         /* the map */
-                     AddrStart,                                      /* start */
-                     AddrStart + cb,                                 /* end */
-                     fFlags);                                        /* flags */
+    if ((fFlags & VM_MAP_WIRE_USER) == VM_MAP_WIRE_USER)
+        rc = vm_map_wire(pVmMap,                                         /* the map */
+                         AddrStart,                                      /* start */
+                         AddrStart + cb,                                 /* end */
+                         fFlags);                                        /* flags */
+    else
+        rc = KERN_SUCCESS;
+
     if (rc == KERN_SUCCESS)
     {
         pMemFreeBSD->Core.u.Lock.R0Process = R0Process;
@@ -743,7 +753,12 @@
     {
         /** @todo: is this needed?. */
         PROC_LOCK(pProc);
-        AddrR3 = round_page((vm_offset_t)pProc->p_vmspace->vm_daddr + lim_max(pProc, RLIMIT_DATA));
+        AddrR3 = round_page((vm_offset_t)pProc->p_vmspace->vm_daddr +
+#if __FreeBSD_version >= 1100077
+                            lim_max_proc(pProc, RLIMIT_DATA));
+#else
+                            lim_max(pProc, RLIMIT_DATA));
+#endif
         PROC_UNLOCK(pProc);
     }
     else
@@ -832,23 +847,6 @@
     switch (pMemFreeBSD->Core.enmType)
     {
         case RTR0MEMOBJTYPE_LOCK:
-        {
-            if (    pMemFreeBSD->Core.u.Lock.R0Process != NIL_RTR0PROCESS
-                &&  pMemFreeBSD->Core.u.Lock.R0Process != (RTR0PROCESS)curproc)
-            {
-                /* later */
-                return NIL_RTHCPHYS;
-            }
-
-            vm_offset_t pb = (vm_offset_t)pMemFreeBSD->Core.pv + ptoa(iPage);
-
-            struct proc    *pProc     = (struct proc *)pMemFreeBSD->Core.u.Lock.R0Process;
-            struct vm_map  *pProcMap  = &pProc->p_vmspace->vm_map;
-            pmap_t pPhysicalMap       = vm_map_pmap(pProcMap);
-
-            return pmap_extract(pPhysicalMap, pb);
-        }
-
         case RTR0MEMOBJTYPE_MAPPING:
         {
             vm_offset_t pb = (vm_offset_t)pMemFreeBSD->Core.pv + ptoa(iPage);
@@ -860,7 +858,8 @@
                 pmap_t pPhysicalMap       = vm_map_pmap(pProcMap);
 
                 return pmap_extract(pPhysicalMap, pb);
-            }
+            } else if (pMemFreeBSD->Core.u.Mapping.R0Process != (RTR0PROCESS)curproc)
+                return NIL_RTHCPHYS;
             return vtophys(pb);
         }
 
