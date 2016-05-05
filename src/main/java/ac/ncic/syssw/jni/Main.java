/*
 * Copyleft 2016, AZQ. All rites reversed.
 */

package ac.ncic.syssw.jni;

import java.nio.ByteBuffer;
import java.util.Random;

public class Main {
	public static void main(String[] args) throws InterruptedException {
		JniNvme.nvmeInitialize();

		ByteBuffer bufferW = JniNvme.allocateHugepageMemory(4096);
		ByteBuffer bufferR = JniNvme.allocateHugepageMemory(4096);

		byte[] data = new byte[4096];
		(new Random()).nextBytes(data);
		bufferW.put(data);

		JniNvme.nvmeWrite(bufferW, 4096, 4096);
		JniNvme.nvmeRead (bufferR, 4096, 4096);

		bufferW.rewind();
		bufferR.rewind();
		System.out.println(bufferR.compareTo(bufferW));

		JniNvme.freeHugepageMemory(bufferW);
		JniNvme.freeHugepageMemory(bufferR);

		JniNvme.nvmeFinalize();
	}
}
