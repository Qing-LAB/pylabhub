# XOP Toolkit — installation instructions

**Important:** This repository does **not** include the WaveMetrics Igor XOP Toolkit distribution due to copyright
and licensing restrictions. You must obtain the toolkit yourself and place its files under:

third_party/XOPToolkit/XOPSupport/


## Copyright / license note (excerpt)
> This manual and the Igor XOP Toolkit are copyrighted by WaveMetrics with all rights reserved. Under copyright laws
> it is illegal for you to copy this manual or the software without written permission from WaveMetrics.
> WaveMetrics gives you permission to make unlimited copies of the software on any number of machines but only for
> your own personal use. You may not copy the software for any other reason. You must ensure that only one copy of the
> software is in use at any given time.
> Manual Revision: October 26, 2021 (8.02)
> © Copyright 2021 WaveMetrics, Inc. All rights reserved.

Before downloading or installing, ensure you accept and comply with WaveMetrics' license terms.

## Expected layout (example)
After extraction the folder should contain the files and structure similar to:

third_party/XOPToolkit/XOPSupport/
├── IGOR.lib
├── IGOR64.lib
├── IgorErrors.h
├── IgorXOP.h
...
├── VC/
│ ├── XOPSupport.lib
│ ├── XOPSupport64.lib
│ └── ...
├── Xcode/
│ ├── libXOPSupport64.a
│ └── ...


