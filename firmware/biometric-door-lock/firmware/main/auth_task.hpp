#pragma once
// Phase 3: the core authentication loop (fingerprint gates face recognition, sequentially - see
// components/auth_fsm). recog_task is a who::recognition::WhoRecognitionCore*; typed void* here so app_main.cpp
// doesn't need the ESP-WHO headers just to start this task.
void auth_task_start(void *recog_task);
