# ResponseReasoningSummaryTextDeltaEvent

Emitted when a delta is added to a reasoning summary text.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.reasoning_summary_text.delta&#x60;.  | 
**item_id** | **str** | The ID of the item this summary text delta is associated with.  | 
**output_index** | **int** | The index of the output item this summary text delta is associated with.  | 
**summary_index** | **int** | The index of the summary part within the reasoning summary.  | 
**delta** | **str** | The text delta that was added to the summary.  | 

## Example

```python
from openapi_client.models.response_reasoning_summary_text_delta_event import ResponseReasoningSummaryTextDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseReasoningSummaryTextDeltaEvent from a JSON string
response_reasoning_summary_text_delta_event_instance = ResponseReasoningSummaryTextDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseReasoningSummaryTextDeltaEvent.to_json())

# convert the object into a dict
response_reasoning_summary_text_delta_event_dict = response_reasoning_summary_text_delta_event_instance.to_dict()
# create an instance of ResponseReasoningSummaryTextDeltaEvent from a dict
response_reasoning_summary_text_delta_event_from_dict = ResponseReasoningSummaryTextDeltaEvent.from_dict(response_reasoning_summary_text_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


