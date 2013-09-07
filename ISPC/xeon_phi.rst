Xeon Phi ハッカソン
###############################################################################
Xeon Phi ハッカソンでは、ISPCつかって性能測定してきました。

ハッカソンでは、5000ファミリーで測定し、所々 7000ファミリーで測定。

ISPCの概要
===============================================================================
ISPCはIntel SPMD (Single Program Multiple Data) Program Compilerの略称です。

http://ispc.github.io/

OSSで開発されており、バックエンドにLLVMを使用しています。

主にSIMDを搭載したCPUやXeon Phiで効率のよいコードを生成できます。

GPGPUはターゲットにしていませんが、ARM NEONはターゲットになっています。

ISPCはC風のISPC言語でkernelを記述し、リンクして実行できます。

問題点として、icpc(インテル(R) C++ コンパイラ)と名前が似ていてややこしい、
開発者がIntelからGoogleに移籍してしまったなどが挙げられます。

Xeon Phiへの対応
===============================================================================
ISPCはXeon Phiにベータ版ですが対応しています。

ISPCを使用する場合、IPSCでコンパイル後、C++ソースコードでemitし、
icpc -micでリコンパイルする必要があります。

gccはMPSSに対応しているのか不明であるため、icpcでリコンパイルするのが良いと思います。

Xeon Phiではnative実行を行いましたが、ソースコードに手を加えればオフロード実行も可能なはず。

MICのオフロード実行用のソースコードを、ISPCから生成可能かどうかは不明です。

ISPCはXeon Phiの512bitのSIMDに対応しており、512bit SIMD向けの拡張クラスを出力します。

Xeon Phiの注意点
===============================================================================
core i7みたいな8threadはもちろん、
Xeon E5-2620 dual coreの24threadでもスケールするようなプログラムをまず用意しましょう！

その後、60コア 240threadのXeon Phiで試す。

scalar演算の単体性能では、Xeon PhiはXeon E5-2620の1/10くらい
Xeon Phiは512bit SIMD命令をきっちり使いきらないと性能はでないのかも。
そういう意味でISPCと相性がいい。

MICはコアがring上に並んでおり、
L2キャッシュ コヒーレントがネックになって性能が出ないことがあるらしい。


ソフトウェアプリフェッチをがつがつ入れたほうがいいらしい。
intelコンパイラは大量のソフトウェアプリフェッチを挿入する。


ISPCでのコンパイル手順
===============================================================================

options ::

  [--emit-asm]      Generate assembly language file as output
  [--emit-c++]      Emit a C++ source file as output
  [--emit-llvm]     Emit LLVM bitode file as output
  [--emit-obj]      Generate object file file as output (default)
  [-h <name>/--header-outfile=<name>] Output filename for header
  [--target=<isa>]      Select target ISA. <isa>={sse2, sse2-x2, sse4, sse4-x2, avx, avx-x2, avx1.1, avx1.1-x2, avx2, avx2-x2, generic-1, generic-4, generic-8, generic-16, generic-32}
  [--arch={x86, x86-64}]    Select target architecture
  [-O0/-O1]       Set optimization level (-O1 is default) O2とO3もO1と同じ。

サンプルプログラムはispc/exampleに入っている。makeすればOK。

基本的には、xxx.ispc をコンパイルしたのち、C++もしくはCで書いたドライバプログラムとリンクして実行します。

::

  $ ispc -O2 -h sample_ispc.h sample.ispc
    sample.o を生成

多くの場合、-hオプションでドライバプログラムから参照するシンボルを定義したヘッダを生成します。

ヘッダファイルはマクロが定義されており、C/C++どちらでもOK

ISPCの詳細は省略。


ISPCのMIC向けコンパイル手順
===============================================================================

ISPCはMIC向けのオブジェクトコードを生成できない。

※  LLVMがMIC(KNC)向けクロスコンパイルをサポートしていないため。

そのためISPCは一度C++ソースコードを生成して、icpcでMIC向けにクロスコンパイルする。

※  LLVMを使用しているため、コンパイル、最適化したのち、bitcodeからC++を生成する。WriteCXXFile()

ispc/example をMIC向けにクロスコンパイルする場合は以下

::

  $ ispc -O2 --emit-c++ -h sample_ispc.h sample.ispc --target=generic-16 --c++-include-file=knc.h -Iintrinsics -o sample_generic16_knc.cpp

MIC向けにクロスコンパイルする場合、--target=generic-16と、--c++-include-file=knc.h が必要。

--target=generic-16 を指定すると、ispc内部で512bit SIMD用のintrinsicsに最適化する。

C++のソースコードを生成する場合、generic-xの指定が必要。アーキテクチャ固有のintrinsicsを使用しない。
MIC向けの場合、generic-16の指定が必要。core i7の場合、generic-4やgeneric-8でOK。

--c++-include-file=knc.h では、icpcの組み込み関数として、vec16やらmm512を定義している。

MICへのクロスコンパイル ::

  $ icpc -O2 --mmic -lpthread -Iobjs -I../intrinsics -I/opt/intel/composer_xe_2013/include main.cpp sample_knc.cpp ../tasksys.cpp -o sample.out

--micオプションは、MIC向けのクロスコンパイルオプション

-lpthread と tasksys.cppは、マルチスレッドの制御を定義していて、デフォルトではpthreadを使用している。

-I/opt/intel/composer_xe_2013 が必要なのは、ispcがintelの組み込み関数を生成しているため。

拡張オプション
===============================================================================

ISPCは1threadにおいてSIMD Unitをフルに使って高速化することに注力し、
その後launch構文でマルチスレッド/マルチコアの並列化を行う。

launch構文はtasksys.cppで定義されており、スレッド制御をいろいろ切り替えれる。

切り替える場合は、コンパイル時にマクロを指定する。
LinuxでのデフォルトはPthreadになっています。

::

  ISPC_USE_CONCRT   //windows default
  ISPC_USE_GCD      //macos default
  ISPC_USE_PTHREADS //linux default
  ISPC_USE_PTHREADS_FULLY_SUBSCRIBED //linux
      -lpthread

  ISPC_USE_TBB_TASK_GROUP    //tbb task
      -std=c++0x -tbb オプションを追加 あとtbbのライブラリのリンクが必要かも。

  ISPC_USE_TBB_PARALLEL_FOR  //tbb parallel
      -std=c++0x -tbb オプションを追加 あとtbbのライブラリのリンクが必要かも。

  ISPC_USE_OMP               //omp
      -openmp

  ISPC_USE_CILK              //cilk


測定方法
===============================================================================
ispc/examplesで行いました。

examples ::

  aobench (NAO_SAMPLES=16, 512*512)
  mandelbrot
  binomial (2M options)
  black-scholes (2M options)
  rt (Ray Tracer sponza)
  volume rendering

ISPC公式のPerformance http://ispc.github.io/perf.html

40 CPU cores ::

  Workload                           , ispc 40cores
  AOBench (2048 * 2048 resolution)   , 182.36x
  Binomial Options (2M options)      ,  63.85x
  Black-Scholes Options (2M Options) ,  83.97x
  Ray Tracer (Sponza dataset)        , 195.67x
  Volume Rendering                   , 243.18x

測定結果
===============================================================================

  =====         =====         =====               =====                 =====
  bench         core i7 2600  Xeon E5-2620(dual)  Xeon Phi 5000ファミリ
                3.4GHz 4core  2.0GHz 12core       1.0GHz 60core
                (MCycle)      (MCycle)            (MCycle)
  =====         =====         =====               =====                 =====
  aobench        443.5         385.74              523.37
  mandelbrot     288.53         63.05               22.97               Xeon Phiで性能向上
  binomial                    1053.28              216.82               Xeon Phiで性能向上
  black-scholes                  5.8                 3.8                Xeon Phiで性能向上
  rt             101.69         32.21               78.31
  volume        4443.99       1020.73              453.11               Xeon Phiで性能向上
  =====         =====         =====               =====                 =====

scalarでの実行を1.0xとした性能向上率

  =====         =====         =====               =====                 =====
  bench         core i7 2600  Xeon E5-2620(dual)  Xeon Phi 5000ファミリ
                3.4GHz 4core  2.0GHz 12core       1.0GHz 60core
  =====         =====         =====               =====                 =====
  aobench        21.90x        53.96x             175.96x
  mandelbrot      9.24x        35.79x             231.26x
  binomial                      8.70x             128.22x
  black-scholes                49.53x             350.71x
  rt             22.92x        60.08x             126.57x
  volume         14.05x        45.05x
  =====         =====         =====               =====                 =====


拡張オプションを使用した測定結果
===============================================================================

todo

  =====         =====         =====               =====                 =====
  bench         core i7 2600  Xeon E5-2620(dual)  Xeon Phi 5000ファミリ
                3.4GHz 4core  2.0GHz 12core       1.0GHz 60core
                (MCycle)      (MCycle)            (MCycle)
  =====         =====         =====               =====                 =====
  aobench       
  mandelbrot  
  binomial    
  black-scholes
  rt            
  volume   
  =====         =====         =====               =====                 =====


ISPCのアーキテクチャ
===============================================================================

Frontendで字句解析と構文解析(flex, bison)

Exprで意味解析とAST変換、ベクトル化と構文に応じたbitcodeへの変換

OptimizerはLLVMのOptimizerをベースに使っており、7-8個の独自Optimizerを追加している。

BackendでLLVMのAPIを叩いて、objを生成するか、c++backendでC++ソースを生成。

他の特徴として、
Builtins Library
Pseudo intrinsics
TaskSystem
がある。

ISPC Optimizer
===============================================================================

::

  CreateImproveMemoryOpsPass() //BasicBlockPass
  // exprのgather scatter を pseudo intrinsicsのgather/scatterに置き換える

  CreateIntrinsicsOptPass()    //BasicBlockPass
  // x86向け、gather,scatterをblendやmaskを使用した高速なload/store命令に置き換える
  CreateVSelMovmskOptPass())   //BasicBlockPass
  // vector select と 定数maskに置換

  CreateGatherCoalescePass()   //BasicBlockPass
  // genericの場合は走らない。
  // 連続するwide loadをgather命令1つに置換
  // 連続するpseudo gather/scatter命令を融合

  CreateReplacePseudoMemoryOpsPass() //BasicBlockPass
  // blendへの置換

  CreateIsCompileTimeConstantPass(true) //BasicBlockPass
  CreateMakeInternalFuncsStaticPass() //ModulePass

Builtins Library
===============================================================================

Math LibraryやstdlibをISPCで書き直しており、デフォルトでISPCの実装を使用する。

Pseudo intrinsics
===============================================================================

Optimizerで言及していたとおり、bitcodeで記述されたpseude intrinsics
(gather, scatter, load, store)が定義されている。

pseudo intrinsicsは、ISA固有の実装と、genericな実装など様々定義されている。
SSE4, AVX, AVX2, NEON, GENERIC4, 8, 16...がある。


TaskSystem
===============================================================================

launch文の処理は、multi threadで実行する。
tasksystem.cppに定義されており、この辺はあまり頑張ってないというか、シンプルな実装になっている。

pthread, omp, tbb task, tbb parallel, cilk の実装が用意されている。

