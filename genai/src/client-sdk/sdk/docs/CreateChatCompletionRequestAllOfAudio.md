# CreateChatCompletionRequestAllOfAudio

Parameters for audio output. Required when audio output is requested with `modalities: [\"audio\"]`. [Learn more](/docs/guides/audio). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**voice** | [**VoiceIdsShared**](VoiceIdsShared.md) |  | 
**format** | **str** | Specifies the output audio format. Must be one of &#x60;wav&#x60;, &#x60;mp3&#x60;, &#x60;flac&#x60;, &#x60;opus&#x60;, or &#x60;pcm16&#x60;.  | 

## Example

```python
from openapi_client.models.create_chat_completion_request_all_of_audio import CreateChatCompletionRequestAllOfAudio

# TODO update the JSON string below
json = "{}"
# create an instance of CreateChatCompletionRequestAllOfAudio from a JSON string
create_chat_completion_request_all_of_audio_instance = CreateChatCompletionRequestAllOfAudio.from_json(json)
# print the JSON string representation of the object
print(CreateChatCompletionRequestAllOfAudio.to_json())

# convert the object into a dict
create_chat_completion_request_all_of_audio_dict = create_chat_completion_request_all_of_audio_instance.to_dict()
# create an instance of CreateChatCompletionRequestAllOfAudio from a dict
create_chat_completion_request_all_of_audio_from_dict = CreateChatCompletionRequestAllOfAudio.from_dict(create_chat_completion_request_all_of_audio_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


