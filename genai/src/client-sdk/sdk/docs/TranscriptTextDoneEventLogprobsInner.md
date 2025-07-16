# TranscriptTextDoneEventLogprobsInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**token** | **str** | The token that was used to generate the log probability.  | [optional] 
**logprob** | **float** | The log probability of the token.  | [optional] 
**bytes** | **List[int]** | The bytes that were used to generate the log probability.  | [optional] 

## Example

```python
from openapi_client.models.transcript_text_done_event_logprobs_inner import TranscriptTextDoneEventLogprobsInner

# TODO update the JSON string below
json = "{}"
# create an instance of TranscriptTextDoneEventLogprobsInner from a JSON string
transcript_text_done_event_logprobs_inner_instance = TranscriptTextDoneEventLogprobsInner.from_json(json)
# print the JSON string representation of the object
print(TranscriptTextDoneEventLogprobsInner.to_json())

# convert the object into a dict
transcript_text_done_event_logprobs_inner_dict = transcript_text_done_event_logprobs_inner_instance.to_dict()
# create an instance of TranscriptTextDoneEventLogprobsInner from a dict
transcript_text_done_event_logprobs_inner_from_dict = TranscriptTextDoneEventLogprobsInner.from_dict(transcript_text_done_event_logprobs_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


