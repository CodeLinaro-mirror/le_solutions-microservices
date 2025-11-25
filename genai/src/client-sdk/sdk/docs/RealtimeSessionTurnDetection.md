# RealtimeSessionTurnDetection

Configuration for turn detection, ether Server VAD or Semantic VAD. This can be set to `null` to turn off, in which case the client must manually trigger model response. Server VAD means that the model will detect the start and end of speech based on audio volume and respond at the end of user speech. Semantic VAD is more advanced and uses a turn detection model (in conjuction with VAD) to semantically estimate whether the user has finished speaking, then dynamically sets a timeout based on this probability. For example, if user audio trails off with \"uhhm\", the model will score a low probability of turn end and wait longer for the user to continue speaking. This can be useful for more natural conversations, but may have a higher latency. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Type of turn detection.  | [optional] [default to 'server_vad']
**eagerness** | **str** | Used only for &#x60;semantic_vad&#x60; mode. The eagerness of the model to respond. &#x60;low&#x60; will wait longer for the user to continue speaking, &#x60;high&#x60; will respond more quickly. &#x60;auto&#x60; is the default and is equivalent to &#x60;medium&#x60;.  | [optional] [default to 'auto']
**threshold** | **float** | Used only for &#x60;server_vad&#x60; mode. Activation threshold for VAD (0.0 to 1.0), this defaults to 0.5. A  higher threshold will require louder audio to activate the model, and  thus might perform better in noisy environments.  | [optional] 
**prefix_padding_ms** | **int** | Used only for &#x60;server_vad&#x60; mode. Amount of audio to include before the VAD detected speech (in  milliseconds). Defaults to 300ms.  | [optional] 
**silence_duration_ms** | **int** | Used only for &#x60;server_vad&#x60; mode. Duration of silence to detect speech stop (in milliseconds). Defaults  to 500ms. With shorter values the model will respond more quickly,  but may jump in on short pauses from the user.  | [optional] 
**create_response** | **bool** | Whether or not to automatically generate a response when a VAD stop event occurs.  | [optional] [default to True]
**interrupt_response** | **bool** | Whether or not to automatically interrupt any ongoing response with output to the default conversation (i.e. &#x60;conversation&#x60; of &#x60;auto&#x60;) when a VAD start event occurs.  | [optional] [default to True]

## Example

```python
from openapi_client.models.realtime_session_turn_detection import RealtimeSessionTurnDetection

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeSessionTurnDetection from a JSON string
realtime_session_turn_detection_instance = RealtimeSessionTurnDetection.from_json(json)
# print the JSON string representation of the object
print(RealtimeSessionTurnDetection.to_json())

# convert the object into a dict
realtime_session_turn_detection_dict = realtime_session_turn_detection_instance.to_dict()
# create an instance of RealtimeSessionTurnDetection from a dict
realtime_session_turn_detection_from_dict = RealtimeSessionTurnDetection.from_dict(realtime_session_turn_detection_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


