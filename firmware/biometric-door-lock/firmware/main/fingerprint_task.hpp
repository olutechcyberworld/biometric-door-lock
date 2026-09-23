#pragma once
// Fingerprint interface (Phase 2). One FreeRTOS task on core 0 owns the AS608; everything else talks to it through
// fingerprint_post(). Console / dashboard commands:
//   f = identify (wait for a finger, 1:N search on the module)   n = enroll into the next free slot
//   p = probe the sensor again                                    X = erase the whole library (send twice within 5 s)
// Output lines (machine readable, consumed by tools/dashboard.py):
//   FPINIT,result=ok|fail,...          FPSTATE,count=N,capacity=M
//   FP,identify,step=place | result=match|nomatch|timeout|error,...
//   FP,enroll,step=start|place1|remove|place2|storing | result=ok|fail,...     FP,empty,result=...
void fingerprint_start();
bool fingerprint_post(char cmd);
