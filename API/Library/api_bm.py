def api_function(action: str):
    if not action:
        return {"message": None}
    if action == "GPS":
        return {"message": "Starting process"}
    elif action == "stop":
        return {"message": "Stopping process"}
    else:
        return {"message": "Unknown action"}
    