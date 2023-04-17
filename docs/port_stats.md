## Is stats collection for ports enabled ?

Two things are required 
  * You should be running the latest sonic-platform-vpp code since port stats support for sonic-vpp code got committed recently.

  * Enable stats collection if not already enabled in the configuration.

The port counters are not enabled by default in the SONiC configuration. If you do not see below configuration in the "show runningconfig all" of SONiC then the fetching of port(interface) counters in SONiC control plane is not enabled.
```
{
    "FLEX_COUNTER_TABLE": {
            "PORT": {
            "FLEX_COUNTER_STATUS": "enable"
        },
        ...
    }
}
```

## How to enable the stats for ports in configuration ?

Create a simple configuration file with FLEX_COUNTER_TABLE having PORT enabled with counter as below

```
cat > CNTR.json
{
    "FLEX_COUNTER_TABLE": {
            "PORT": {
            "FLEX_COUNTER_STATUS": "enable",
	    "POLL_INTERVAL": "20000"
        }
    }
}
Ctrl-D
```

Load the above cntr.json file using config load command
```
sudo config load -y cntr.json
```

Now run "show interface counter" few times to see the updated counters.