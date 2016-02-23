package ac.ncic.syssw.jni;

public final class JniNvme {
	static {
		System.loadLibrary("jninvme");
	}

	public static native int spdkIdentify();
}
