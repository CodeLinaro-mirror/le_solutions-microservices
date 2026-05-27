# ResponseStreamEvent


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.audio.delta&#x60;.  | 
**delta** | **str** | The text delta that was added.  | 
**output_index** | **int** | The index of the output item that the web search call is associated with.  | 
**code** | **str** | The error code.  | 
**code_interpreter_call** | [**CodeInterpreterToolCall**](CodeInterpreterToolCall.md) |  | 
**response** | [**Response**](Response.md) |  | 
**item_id** | **str** | Unique ID for the output item associated with the web search call.  | 
**content_index** | **int** | The index of the content part that the text content is finalized.  | 
**part** | [**ResponseReasoningSummaryPartDoneEventPart**](ResponseReasoningSummaryPartDoneEventPart.md) |  | 
**message** | **str** | The error message.  | 
**param** | **str** | The error parameter.  | 
**arguments** | **str** | The function-call arguments. | 
**item** | [**OutputItem**](OutputItem.md) |  | 
**summary_index** | **int** | The index of the summary part within the reasoning summary.  | 
**text** | **str** | The text content that is finalized.  | 
**refusal** | **str** | The refusal text that is finalized.  | 
**annotation_index** | **int** | The index of the annotation that was added.  | 
**annotation** | [**Annotation**](Annotation.md) |  | 

## Example

```python
from openapi_client.models.response_stream_event import ResponseStreamEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseStreamEvent from a JSON string
response_stream_event_instance = ResponseStreamEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseStreamEvent.to_json())

# convert the object into a dict
response_stream_event_dict = response_stream_event_instance.to_dict()
# create an instance of ResponseStreamEvent from a dict
response_stream_event_from_dict = ResponseStreamEvent.from_dict(response_stream_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


