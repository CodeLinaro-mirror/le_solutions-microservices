# InputAudio

An audio input to the model. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_audio&#x60;.  | 
**data** | **str** | Base64-encoded audio data.  | 
**format** | **str** | The format of the audio data. Currently supported formats are &#x60;mp3&#x60; and &#x60;wav&#x60;.  | 

## Example

```python
from openapi_client.models.input_audio import InputAudio

# TODO update the JSON string below
json = "{}"
# create an instance of InputAudio from a JSON string
input_audio_instance = InputAudio.from_json(json)
# print the JSON string representation of the object
print(InputAudio.to_json())

# convert the object into a dict
input_audio_dict = input_audio_instance.to_dict()
# create an instance of InputAudio from a dict
input_audio_from_dict = InputAudio.from_dict(input_audio_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


