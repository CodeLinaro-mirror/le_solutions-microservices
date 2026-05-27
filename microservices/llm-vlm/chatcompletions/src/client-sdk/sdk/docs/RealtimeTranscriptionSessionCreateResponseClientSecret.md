# RealtimeTranscriptionSessionCreateResponseClientSecret

Ephemeral key returned by the API. Only present when the session is created on the server via REST API. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**value** | **str** | Ephemeral key usable in client environments to authenticate connections to the Realtime API. Use this in client-side environments rather than a standard API token, which should only be used server-side.  | 
**expires_at** | **int** | Timestamp for when the token expires. Currently, all tokens expire after one minute.  | 

## Example

```python
from openapi_client.models.realtime_transcription_session_create_response_client_secret import RealtimeTranscriptionSessionCreateResponseClientSecret

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeTranscriptionSessionCreateResponseClientSecret from a JSON string
realtime_transcription_session_create_response_client_secret_instance = RealtimeTranscriptionSessionCreateResponseClientSecret.from_json(json)
# print the JSON string representation of the object
print(RealtimeTranscriptionSessionCreateResponseClientSecret.to_json())

# convert the object into a dict
realtime_transcription_session_create_response_client_secret_dict = realtime_transcription_session_create_response_client_secret_instance.to_dict()
# create an instance of RealtimeTranscriptionSessionCreateResponseClientSecret from a dict
realtime_transcription_session_create_response_client_secret_from_dict = RealtimeTranscriptionSessionCreateResponseClientSecret.from_dict(realtime_transcription_session_create_response_client_secret_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


