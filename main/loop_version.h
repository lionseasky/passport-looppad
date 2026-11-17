// main/loop_version.h —— 发布版本号(唯一来源)。
//
// 只改这里:构建会把它写进启动日志,镜像里因此能直接 grep 到版本,
// 用来确认设备上跑的到底是不是 releases/ 里那一份。
#pragma once

#define LOOP_PAD_VERSION "0.1"
