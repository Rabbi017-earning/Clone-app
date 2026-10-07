package top.niunaijun.blackboxa.view.proxy

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackbox.core.system.CloneDevice
import top.niunaijun.blackbox.core.system.CloneProxy

/** Per-clone proxy (+DNS through proxy) and device model. Applies next time the clone starts. */
class ProxyActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val userId = intent.getIntExtra("userId", 0)
        val pkg = intent.getStringExtra("pkg") ?: return finish()
        title = "Settings: $pkg"
        val ctx = BlackBoxCore.getContext()

        fun field(hint: String, type: Int = InputType.TYPE_CLASS_TEXT) =
            EditText(this).apply { this.hint = hint; inputType = type }
        fun label(t: String) = TextView(this).apply { text = t; setPadding(0, 32, 0, 0) }

        val types = arrayOf("HTTP", "SOCKS5")
        val typeSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@ProxyActivity, android.R.layout.simple_spinner_dropdown_item, types)
        }
        val host = field("Proxy host (empty = no proxy)")
        val port = field("Port", InputType.TYPE_CLASS_NUMBER)
        val user = field("Username (optional)")
        val pass = field("Password (optional)", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD)
        val dns = Switch(this).apply { text = "DNS through proxy"; isChecked = true }

        val cur = CloneProxy.remoteGet(ctx, userId, pkg)
        host.setText(cur.getString("host", ""))
        if (cur.getInt("port", 0) > 0) port.setText(cur.getInt("port").toString())
        user.setText(cur.getString("user", ""))
        pass.setText(cur.getString("pass", ""))
        typeSpinner.setSelection(if (cur.getString("type", "HTTP") == "SOCKS5") 1 else 0)
        if (cur.getString("host", "").isNullOrEmpty().not()) dns.isChecked = cur.getBoolean("dns", false)

        val presets = CloneDevice.PRESETS
        val deviceSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@ProxyActivity, android.R.layout.simple_spinner_dropdown_item, presets.map { it[1] })
        }
        val curDev = CloneDevice.remoteGet(ctx, userId, pkg)
        deviceSpinner.setSelection(presets.indexOfFirst { it[0] == curDev }.coerceAtLeast(0))

        val save = Button(this).apply {
            text = "Save"
            setOnClickListener {
                CloneProxy.remoteSet(
                    ctx, userId, pkg,
                    host.text.toString().trim(), port.text.toString().toIntOrNull() ?: 0,
                    types[typeSpinner.selectedItemPosition], user.text.toString(), pass.text.toString(),
                    dns.isChecked
                )
                CloneDevice.remoteSet(ctx, userId, pkg, presets[deviceSpinner.selectedItemPosition][0])
                Toast.makeText(this@ProxyActivity, "Saved. Stop and reopen the clone to apply.", Toast.LENGTH_LONG).show()
                finish()
            }
        }

        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(40, 40, 40, 40) }
        root.addView(label("Proxy"))
        listOf(typeSpinner, host, port, user, pass, dns).forEach { root.addView(it) }
        root.addView(label("Device model shown to the app"))
        root.addView(deviceSpinner)
        root.addView(save)
        setContentView(ScrollView(this).apply { addView(root) })
    }

    companion object {
        fun start(ctx: Context, userId: Int, pkg: String) {
            ctx.startActivity(Intent(ctx, ProxyActivity::class.java).putExtra("userId", userId).putExtra("pkg", pkg))
        }
    }
}
