//
// PLM
// Copyright
// PLM
//

using System;
using System.Runtime.InteropServices;

namespace System {
	[StructLayout(LayoutKind.Sequential)]
	internal struct MonoGHashTableSlotNoGC {
		IntPtr key;
		IntPtr val;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct MonoGHashTableSlotK {
		object key;
		IntPtr val;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct MonoGHashTableSlotV {
		IntPtr key;
		object val;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct MonoGHashTableSlotKV {
		object key;
		object val;
	}
}
