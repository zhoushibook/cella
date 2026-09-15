#pragma once

// ─────────────────────────────────────────────────────────────────────────
// 控制台按 UTF-8 显示中文（仅 Windows 需要）。
//
// 为什么需要：Windows 控制台默认代码页是 GBK(936)，而本模块源码编译时带
// /utf-8、字符串字面量是 UTF-8 字节。代码页不匹配时中文会显示成乱码，
// 部分字节还会被吞掉导致整行截断（实测：PowerShell 里 "插入 5000 行，耗时
// 3.56 ms，占用 31 页" 变成 "鈶?鎻掑叆 … 鍗犵敤"）。
// 与 cella_db / cella_sql / cella_client 的 main 保持同一做法：启动时把
// 控制台输出代码页切到 65001(UTF-8)。重定向到文件时不受影响（字节流本来就是
// UTF-8，代码页只影响控制台渲染）。
//
// 为什么不直接 include <windows.h>：它会定义 DELETE / IN / OUT 等宏，打碎
// 本项目方言头里的标识符（AST 的 DELETE 语句枚举、token 的 IN 关键字）。
// 因此沿用其它模块的做法：只手工声明这一个 API。
// ─────────────────────────────────────────────────────────────────────────

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif

namespace cella::storage {

// 在 main 开头调用一次即可；非 Windows 平台是空操作。
inline void EnableUtf8Console() {
#if defined(_WIN32)
  SetConsoleOutputCP(65001);   // CP_UTF8
#endif
}

}  // namespace cella::storage
