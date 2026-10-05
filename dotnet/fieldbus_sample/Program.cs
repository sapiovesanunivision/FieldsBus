// C# version of fb_device_demo: the same app on any fieldbus.
//   input area  (PLC -> PC)  printed when the PLC changes it
//   output area (PC -> PLC)  = echo of the input area, bytes 0..3 = heartbeat
// Usage: SoftFieldbusSample [eip|modbus-tcp|modbus-udp|modbus] [bind-ip]
using System;
using System.Threading;
using SoftFieldbus;

var transport = FieldbusTransport.EtherNetIP;
if (args.Length > 0)
{
    transport = args[0].ToLowerInvariant() switch
    {
        "eip" => FieldbusTransport.EtherNetIP,
        "modbus-tcp" => FieldbusTransport.ModbusTcp,
        "modbus-udp" => FieldbusTransport.ModbusUdp,
        "modbus" => FieldbusTransport.ModbusTcpUdp,
        _ => throw new ArgumentException($"unknown transport {args[0]}"),
    };
}

var cfg = new FieldbusDeviceConfig
{
    Transport = transport,
    InputSize = 64,   // PLC -> PC
    OutputSize = 64,  // PC -> PLC
    ModbusUnitId = 1,
    ProductName = "SoftFieldbus C# Sample",
};
if (args.Length > 1)
    cfg.BindAddress = args[1];

using var dev = new FieldbusDevice(cfg);

// Raised on the network thread. In WPF/WinUI marshal to the UI thread
// (Dispatcher.BeginInvoke / DispatcherQueue.TryEnqueue) before touching controls.
dev.Log += (_, e) => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} {e.Message}");
dev.StateChanged += (_, e) => Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} state: {e.State}");
dev.InputsChanged += (_, e) =>
    Console.WriteLine($"{DateTime.Now:HH:mm:ss.fff} inputs (PLC -> PC) " +
                      BitConverter.ToString(e.Data, 0, Math.Min(16, e.Data.Length)));

dev.Start();
Console.WriteLine($"Transport {dev.Transport}: input {dev.InputSize} B (PLC -> PC), " +
                  $"output {dev.OutputSize} B (PC -> PLC). Ctrl+C to quit.");

using var quit = new ManualResetEventSlim();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; quit.Set(); };

// The application: identical for every transport.
var outputs = new byte[dev.OutputSize];
uint heartbeat = 0;
while (!quit.Wait(10))
{
    byte[] inputs = dev.IoRead();
    Array.Copy(inputs, outputs, Math.Min(inputs.Length, outputs.Length));
    BitConverter.GetBytes(++heartbeat).CopyTo(outputs, 0);
    dev.IoWrite(0, outputs);
}

dev.Stop();
Console.WriteLine("stopped");
