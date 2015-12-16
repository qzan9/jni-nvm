package ac.ncic.syssw.jni.spdk;

public final class JniSpdk {
	static {
		System.loadLibrary("jnispdk");
	}

	public static native int spdkIdentify();
}
