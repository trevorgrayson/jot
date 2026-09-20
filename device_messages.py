#
# moved to tg>pearl/jot
#
from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from threading import Lock
from typing import Any

from flask import Blueprint, current_app, jsonify, request


messages_bp = Blueprint("device_messages", __name__, url_prefix="/api/v1")

_store_lock = Lock()


def _store_path() -> Path:
    configured_path = current_app.config.get("DEVICE_MESSAGES_FILE")

    if configured_path:
        return Path(configured_path)

    return Path(
        os.environ.get("DEVICE_MESSAGES_FILE", "data/device_messages.json")
    )


def _empty_store() -> dict[str, Any]:
    return {
        "messages": [],
        "seen": [],
    }


def _load_store(path: Path) -> dict[str, Any]:
    if not path.exists():
        return _empty_store()

    with path.open("r", encoding="utf-8") as handle:
        store = json.load(handle)

    if not isinstance(store, dict):
        raise ValueError("message store root must be an object")

    messages = store.get("messages", [])
    seen = store.get("seen", [])

    if not isinstance(messages, list) or not isinstance(seen, list):
        raise ValueError("message store has invalid shape")

    return {
        "messages": messages,
        "seen": seen,
    }


def _save_store(path: Path, store: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(
        "w",
        encoding="utf-8",
        dir=path.parent,
        delete=False,
    ) as handle:
        json.dump(store, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary_path = Path(handle.name)

    temporary_path.replace(path)


def _validate_payload(payload: Any) -> tuple[str, list[dict[str, str]]]:
    if not isinstance(payload, dict):
        raise ValueError("JSON body must be an object")

    device_id = payload.get("device_id")

    if not isinstance(device_id, str) or not device_id.strip():
        raise ValueError("device_id is required")

    raw_messages = payload.get("messages")

    if not isinstance(raw_messages, list):
        raise ValueError("messages must be a list")

    messages: list[dict[str, str]] = []

    for index, message in enumerate(raw_messages):
        if not isinstance(message, dict):
            raise ValueError(f"messages[{index}] must be an object")

        message_id = message.get("id")
        text = message.get("text")

        if not isinstance(message_id, str) or not message_id.strip():
            raise ValueError(f"messages[{index}].id is required")

        if not isinstance(text, str) or not text.strip():
            raise ValueError(f"messages[{index}].text is required")

        messages.append(
            {
                "id": message_id.strip(),
                "text": text.strip(),
            }
        )

    return device_id.strip(), messages


def _authorized() -> bool:
    expected_token = current_app.config.get("DEVICE_API_TOKEN")

    if not expected_token:
        expected_token = os.environ.get("DEVICE_API_TOKEN")

    if not expected_token:
        return True

    authorization = request.headers.get("Authorization", "")
    scheme, _, token = authorization.partition(" ")

    return scheme.lower() == "bearer" and token == expected_token


@messages_bp.post("/messages")
def accept_messages():
    # Temporarily allow pearl/jot messages without a bearer token.
    # if not _authorized():
    #     return jsonify({"error": "unauthorized"}), 401

    payload = request.get_json(silent=True)

    try:
        device_id, incoming_messages = _validate_payload(payload)
    except ValueError as error:
        return jsonify({"error": str(error)}), 400

    received_at = datetime.now(timezone.utc).isoformat()
    path = _store_path()

    with _store_lock:
        try:
            store = _load_store(path)
        except (OSError, json.JSONDecodeError, ValueError) as error:
            current_app.logger.exception("Unable to load message store")
            return jsonify({"error": f"unable to load message store: {error}"}), 500

        seen = set(store["seen"])
        accepted = 0

        for message in incoming_messages:
            seen_key = f"{device_id}:{message['id']}"

            if seen_key in seen:
                continue

            store["messages"].append(
                {
                    "device_id": device_id,
                    "id": message["id"],
                    "text": message["text"],
                    "received_at": received_at,
                }
            )

            store["seen"].append(seen_key)
            seen.add(seen_key)
            accepted += 1

        try:
            _save_store(path, store)
        except OSError as error:
            current_app.logger.exception("Unable to save message store")
            return jsonify({"error": f"unable to save message store: {error}"}), 500

    return jsonify(
        {
            "accepted": accepted,
            "total": len(store["messages"]),
        }
    )


@messages_bp.route("/messages", methods=["GET"])
def list_messages():
    # Temporarily allow pearl/jot reads without a bearer token.
    # if not _authorized():
    #     return jsonify({"error": "unauthorized"}), 401

    path = _store_path()

    with _store_lock:
        try:
            store = _load_store(path)
        except (OSError, json.JSONDecodeError, ValueError) as error:
            current_app.logger.exception("Unable to load message store")
            return jsonify({"error": f"unable to load message store: {error}"}), 500

    return jsonify(
        {
            "messages": store["messages"],
            "total": len(store["messages"]),
        }
    )
