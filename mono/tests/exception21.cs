using System;
using System.Threading;

public class Program {

	public static void Main (string[] args) {
		int caughts = 0;
		try {
			try {
				throw new Exception ();
			} finally {
				throw new Exception ();
			}
		} catch (Exception) {
			caughts++;
			Console.WriteLine ("Caught");
		}
		if (caughts != 1)
			Environment.Exit (1);
		Console.WriteLine ("Exit");
	}
} 
