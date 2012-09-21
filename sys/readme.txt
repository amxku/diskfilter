========================================================================
       NT Kernel Driver : diskfilter
========================================================================

http://os.sebug.net/

> 支持2000 xp 2003 vista win7
> 不在磁盘上产生任何临时文件
> 超优化的算法，在保护的情况下，操作硬盘，跟没有保护的情况下，速度一样, 不伤硬盘
> 模拟硬件还原卡工作原理，稳定快速
> 密码保护，用户登录后可以任意配置还原选项
> 支持只保护系统盘，和全盘保护, 支持多硬盘
> 保护MBR，加入防机器狗模块，用户可以手头关闭拦截第三方驱动的功能，比如用来完一些带驱动保护的游戏(以后改成白名单)
> 所有功能，集成到一个驱动文件里，全绿色免安装，一共一个文件
> 界面简洁，操作方便，没任何副作用，呵呵, 我诚认界面是学习迅闪的，以前用习惯它了，不过它不防狗
> 更改了下算法，应付内存特别小的情况 V1.3
> 当然，以上介绍有部分夸大之词
> 加载外来驱动自动弹对话框让用户选择, vista,win7系统自动关闭驱动拦截(vista&win7有驱动验证)

QuickSYS has created this diskfilter SYS for you.  

This file contains a summary of what you will find in each of the files that
make up your application.

diskfilter.dsp
    This file (the project file) contains information at the project level and
    is used to build a single project or subproject. Other users can share the
    project (.dsp) file, but they should export the makefiles locally.

diskfilter.c
    This is the main SYS source file.

diskfilter.h
    This file contains your SYS definition.

dbghelp.h
	This file contains some useful macros.

/////////////////////////////////////////////////////////////////////////////
Other notes:

AppWizard uses "TODO:" to indicate parts of the source code you
should add to or customize.


/////////////////////////////////////////////////////////////////////////////
