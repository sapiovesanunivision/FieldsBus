// C# version of eip_adapter_demo: inputs = echo of outputs, bytes 0..3 = heartbeat.
// Usage: SoftEipSample [bind-ip]
using System;
using System.Threading;
using SoftFieldbus;

var cfg = new EipAdapterConfig
{
    InputSize = 32,
    OutputSize = 32,
    ProductName = "SoftEIP C# Sample",
};
if (args.Length > 0)
    cfg.BindAddress = args[0];

using var adapter = new EipAdapter(cfg);

// Raised on the adapter's network thread. In WPF/WinUI marshal to the UI thread
// (Dispatcher.BeginInvoke / DispatcherQueue.TryEnqueue) before touching controls.
adapter.Log += (_, e) => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {e.Message}");
adapter.ConnectionChanged += (_, e) =>
    Console.WriteLine(e.Connected ? "PLC connected (exclusive owner)" : "PLC disconnected");
adapter.OutputsChanged += (_, e) =>
    Console.WriteLine($"outputs [{(e.PlcRun ? "RUN " : "IDLE")}] " +
                      BitConverter.ToString(e.Data, 0, Math.Min(16, e.Data.Length)));

adapter.Start();
Console.WriteLine($"Running: input {cfg.InputSize} B (instance {cfg.InputInstance}), " +
                  $"output {cfg.OutputSize} B (instance {cfg.OutputInstance}). Ctrl+C to quit.");

using var quit = new ManualResetEventSlim();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; quit.Set(); };

var inputs = new byte[cfg.InputSize];
uint heartbeat = 0;
while (!quit.Wait(10))
{
    byte[] outputs = adapter.GetOutputs();
    Array.Copy(outputs, inputs, Math.Min(inputs.Length, outputs.Length));
    BitConverter.GetBytes(++heartbeat).CopyTo(inputs, 0);
    adapter.SetInputs(inputs);
}

adapter.Stop();
Console.WriteLine("stopped");
