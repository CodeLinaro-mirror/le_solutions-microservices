# CreateTranscriptionRequest

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file** | **bytearray** | The audio file object (not file name) to transcribe, in one of these formats: flac, mp3, mp4, mpeg, mpga, m4a, ogg, wav, or webm.  |
**model** | [**CreateTranscriptionRequestModel**](CreateTranscriptionRequestModel.md) |  |
**language** | **str** | The language of the input audio. Supplying the input language in ISO-639-1 (e.g. &#x60;en&#x60;) format will improve accuracy and latency.  | [optional]
**prompt** | **str** | An optional text to guide the model&#39;s style or continue a previous audio segment. The [prompt](/docs/guides/speech-to-text#prompting) should match the audio language.  | [optional]
**response_format** | [**AudioResponseFormat**](AudioResponseFormat.md) |  | [optional] [default to AudioResponseFormat.JSON]
**temperature** | **float** | The sampling temperature, between 0 and 1. Higher values like 0.8 will make the output more random, while lower values like 0.2 will make it more focused and deterministic. If set to 0, the model will use log probability to automatically increase the temperature until certain thresholds are hit.  | [optional] [default to 0]
**include** | [**List[TranscriptionInclude]**](TranscriptionInclude.md) | Additional information to include in the transcription response.  &#x60;logprobs&#x60; will return the log probabilities of the tokens in the  response to understand the model&#39;s confidence in the transcription.  &#x60;logprobs&#x60; only works with response_format set to &#x60;json&#x60;  | [optional]
**timestamp_granularities** | **List[str]** | The timestamp granularities to populate for this transcription. &#x60;response_format&#x60; must be set &#x60;verbose_json&#x60; to use timestamp granularities. Either or both of these options are supported: &#x60;word&#x60;, or &#x60;segment&#x60;. Note: There is no additional latency for segment timestamps, but generating word timestamps incurs additional latency.  | [optional] [default to ["segment"]]
**stream** | **bool** | If set to true, the model response data will be streamed to the client as it is generated using server-sent events.  See the [Streaming section of the Speech-to-Text guide](/docs/guides/speech-to-text?lang&#x3D;curl#streaming-transcriptions) for more information.  | [optional] [default to False]

## Example

```python
from openapi_client.models.create_transcription_request import CreateTranscriptionRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateTranscriptionRequest from a JSON string
create_transcription_request_instance = CreateTranscriptionRequest.from_json(json)
# print the JSON string representation of the object
print(CreateTranscriptionRequest.to_json())

# convert the object into a dict
create_transcription_request_dict = create_transcription_request_instance.to_dict()
# create an instance of CreateTranscriptionRequest from a dict
create_transcription_request_from_dict = CreateTranscriptionRequest.from_dict(create_transcription_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)

