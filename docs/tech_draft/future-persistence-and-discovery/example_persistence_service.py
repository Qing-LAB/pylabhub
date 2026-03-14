import asyncio
import numpy as np
# from api import Hub, StreamSpec
# from persistence_service import PersistenceService

async def demo():
    hub = Hub("hub-1")

    # Register persistence
    svc = PersistenceService(base_dir="./persist_out")
    hub.register_service(svc)

    # Start a session
    await hub.open_session(token="dev")

    # Create a numeric stream
    await hub.create_stream(
        name="camera_raw",
        spec=StreamSpec(name="camera_raw", dtype="uint8", shape=(480, 640), units=None, encoding="application/x-numpy")
    )

    # Emit a state + an event
    await hub.emit_state(scope="instrument/camera", fields={"exposure_ms": 10, "gain": 2})
    await hub.emit_event(name="capture_started", payload={"sequence": 1})

    # Send a data frame (simulate a 480x640 uint8 image)
    frame = np.random.randint(0, 255, size=(480, 640), dtype=np.uint8)
    await hub.send_data(
        stream="camera_raw",
        header={"dtype": "uint8", "shape": (480, 640)},
        buffer=frame.tobytes()
    )

    # Give the service loop a tick to flush
    await asyncio.sleep(0.05)

    # Clean shutdown
    await svc.close()
    await hub.close_session()

if __name__ == "__main__":
    asyncio.run(demo())
