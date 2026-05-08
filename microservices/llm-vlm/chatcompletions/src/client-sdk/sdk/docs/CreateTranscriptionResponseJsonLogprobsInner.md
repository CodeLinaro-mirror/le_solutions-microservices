# CreateTranscriptionResponseJsonLogprobsInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**token** | **str** | The token in the transcription. | [optional] 
**logprob** | **float** | The log probability of the token. | [optional] 
**bytes** | **List[float]** | The bytes of the token. | [optional] 

## Example

```python
from openapi_client.models.create_transcription_response_json_logprobs_inner import CreateTranscriptionResponseJsonLogprobsInner

# TODO update the JSON string below
json = "{}"
# create an instance of CreateTranscriptionResponseJsonLogprobsInner from a JSON string
create_transcription_response_json_logprobs_inner_instance = CreateTranscriptionResponseJsonLogprobsInner.from_json(json)
# print the JSON string representation of the object
print(CreateTranscriptionResponseJsonLogprobsInner.to_json())

# convert the object into a dict
create_transcription_response_json_logprobs_inner_dict = create_transcription_response_json_logprobs_inner_instance.to_dict()
# create an instance of CreateTranscriptionResponseJsonLogprobsInner from a dict
create_transcription_response_json_logprobs_inner_from_dict = CreateTranscriptionResponseJsonLogprobsInner.from_dict(create_transcription_response_json_logprobs_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


