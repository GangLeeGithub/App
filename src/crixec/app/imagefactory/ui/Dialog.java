package crixec.app.imagefactory.ui;
import android.content.Context;
import android.support.v7.app.AlertDialog;

public class Dialog
{
	public static AlertDialog.Builder build(Context ctx){
		return new AlertDialog.Builder(ctx);
	}
}
