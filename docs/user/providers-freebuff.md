# Freebuff

Install Freebuff 0.2.34 or newer on the machine running the T3 Code server. Run
`freebuff` there and complete its setup before using it from T3 Code.

Open **Settings → Providers** to check Freebuff's status or set its **Binary path**,
then choose **Freebuff** in the model picker. Freebuff is enabled by default and
can be disabled in provider settings.

Messages and tool activity appear in normal threads. Progress arrives at
Freebuff's periodic checkpoints, so responses may update in larger chunks.

Freebuff uses the model selected in its CLI. It runs in **Full access**, which
allows it to edit files and run commands without asking for approval.

T3's built-in agent tools are unavailable with Freebuff. Choose another provider
for generated thread titles, commit messages, and pull request descriptions.

## Use Freebuff with Pi

This checkout also includes an experimental Pi extension that uses Freebuff as
the model backend while Pi runs its tools, including T3's built-in agent tools.
It was tested with Pi 1.4.0 and Freebuff 0.2.34 on Linux.

After installing this checkout's dependencies, enable **Pi** in **Settings →
Providers** and set its launch arguments to:

```sh
--extension /absolute/path/to/t3code/packages/freebuff-pi/src/freebuff-provider.ts --provider freebuff-pi --model freebuff
```

Replace the extension path with your checkout's absolute path. Refresh Pi and
select **Freebuff**. If needed, add a custom model with slug
`freebuff-pi/freebuff` and name `Freebuff`. Keep the checkout and its dependencies
available to the server. Native Freebuff login is still required; Pi does not
need a separate API key for this model.

Set `PI_CODING_AGENT_DIR` in Pi's provider environment to use a separate Pi
configuration directory. Freebuff's copied login and session data are stored in
its `freebuff` subdirectory; `FREEBUFF_PI_STATE_DIR` can select another private
directory. `FREEBUFF_BINARY` can select the native Freebuff executable.

The extension accepts text input and reports no token usage or cost. Responses
arrive in chunks, and invalid tool-response JSON gets at most two correction
requests before the turn fails. Background thread titles, commit messages, and
pull request descriptions still require another model.

Freebuff's native tools remain available internally. The extension instructs it
to request Pi tools and stops when it observes native tool execution, but this
cannot prevent a native side effect that has already happened. Use a disposable
project while testing this integration.
